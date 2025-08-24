/**
 * Thread Pool-based HTTP Server Implementation
 * Optimized for C10K with minimal thread overhead
 */

#include "thread_server.h"
#include "../common/http.h"
#include "../common/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/resource.h>

/* Configuration for C10K optimization */
enum
{
    kMaxWorkerThreads = 200,       /* Optimal thread pool size */
    kQueueSize = 10000,            /* Connection queue size */
    kMaxRequestSize = 4096,        /* Reduced for memory efficiency */
    kMaxPathSize = 1024,           /* Path buffer */
    kMaxHeaderSize = 512,          /* Response header buffer */
    kFileBufferSize = 32768,       /* 32KB for file I/O */
    kListenBacklog = 10000,        /* Match system somaxconn */
    kThreadStackSize = 128 * 1024, /* 128KB stack (reduced) */
    kSocketTimeoutSec = 10,        /* Shorter timeout for C10K */
    kKeepAliveMax = 100,           /* Max requests per connection */
    kKeepAliveTimeout = 5          /* Keep-alive timeout in seconds */
};

/* Connection queue node */
typedef struct Connection
{
    int fd;
    const char *doc_root;
    struct Connection *next;
} Connection;

/* Thread pool structure */
typedef struct
{
    pthread_t *threads;
    int num_threads;

    /* Connection queue */
    Connection *queue_head;
    Connection *queue_tail;
    int queue_size;

    /* Synchronization */
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    bool shutdown;

    /* Statistics */
    uint64_t total_requests;
    uint64_t active_connections;
    uint64_t total_connections;
} ThreadPool;

/* Global thread pool */
static ThreadPool *g_pool = NULL;

/* Function prototypes */
static ThreadPool *thread_pool_create(int num_threads);
static void thread_pool_destroy(ThreadPool *pool);
static void thread_pool_add_connection(ThreadPool *pool, int fd, const char *doc_root);
static void *worker_thread(void *arg);
static void handle_connection(int fd, const char *doc_root);
static int process_request(int fd, const char *doc_root, bool *keep_alive);
static int send_file_response(int client_fd, const char *file_path, bool keep_alive);
static int send_error_response(int client_fd, int status_code, bool keep_alive);
static int create_server_socket(const char *bind_addr, int port);
static void configure_socket_options(int socket_fd);
static int increase_limits(void);

/**
 * Main server entry point with thread pool
 */
int run_thread_server(const char *bind_addr, int port, const char *doc_root)
{
    if (!doc_root)
    {
        fprintf(stderr, "Error: document root is required\n");
        return -1;
    }

    /* Increase system limits for C10K */
    if (increase_limits() < 0)
    {
        fprintf(stderr, "Warning: Could not increase limits\n");
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Create thread pool */
    g_pool = thread_pool_create(kMaxWorkerThreads);
    if (!g_pool)
    {
        fprintf(stderr, "Failed to create thread pool\n");
        return -1;
    }

    /* Create server socket */
    int server_fd = create_server_socket(bind_addr, port);
    if (server_fd < 0)
    {
        thread_pool_destroy(g_pool);
        return -1;
    }

    fprintf(stderr, "Thread pool server listening on %s:%d (doc_root: %s)\n",
            bind_addr ? bind_addr : "0.0.0.0", port, doc_root);
    fprintf(stderr, "Thread pool size: %d workers\n", kMaxWorkerThreads);

    /* Main accept loop */
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE)
            {
                /* Too many open files, wait a bit */
                usleep(1000);
                continue;
            }
            perror("accept");
            continue;
        }

        /* Configure client socket */
        configure_socket_options(client_fd);

        /* Add to thread pool queue */
        thread_pool_add_connection(g_pool, client_fd, doc_root);

        /* Print stats periodically */
        static time_t last_stats = 0;
        time_t now = time(NULL);
        if (now - last_stats >= 10)
        {
            fprintf(stderr, "Stats: queue=%d active=%llu total=%llu requests=%llu\n",
                    g_pool->queue_size,
                    (unsigned long long)g_pool->active_connections,
                    (unsigned long long)g_pool->total_connections,
                    (unsigned long long)g_pool->total_requests);
            last_stats = now;
        }
    }

    /* Cleanup (unreachable) */
    close(server_fd);
    thread_pool_destroy(g_pool);
    return 0;
}

/**
 * Create thread pool
 */
static ThreadPool *thread_pool_create(int num_threads)
{
    ThreadPool *pool = calloc(1, sizeof(ThreadPool));
    if (!pool)
        return NULL;

    pool->num_threads = num_threads;
    pool->threads = calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads)
    {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);

    /* Configure thread attributes */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kThreadStackSize);

    /* Create worker threads */
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], &attr, worker_thread, pool) != 0)
        {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            pool->num_threads = i;
            break;
        }
    }

    pthread_attr_destroy(&attr);
    return pool;
}

/**
 * Destroy thread pool
 */
static void thread_pool_destroy(ThreadPool *pool)
{
    if (!pool)
        return;

    /* Signal shutdown */
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Wait for threads */
    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    /* Cleanup queue */
    while (pool->queue_head)
    {
        Connection *conn = pool->queue_head;
        pool->queue_head = conn->next;
        close(conn->fd);
        free(conn);
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    free(pool->threads);
    free(pool);
}

/**
 * Add connection to thread pool queue
 */
static void thread_pool_add_connection(ThreadPool *pool, int fd, const char *doc_root)
{
    Connection *conn = malloc(sizeof(Connection));
    if (!conn)
    {
        close(fd);
        return;
    }

    conn->fd = fd;
    conn->doc_root = doc_root;
    conn->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);

    /* Check queue size limit */
    if (pool->queue_size >= kQueueSize)
    {
        pthread_mutex_unlock(&pool->queue_mutex);
        close(fd);
        free(conn);
        return;
    }

    /* Add to queue */
    if (pool->queue_tail)
    {
        pool->queue_tail->next = conn;
    }
    else
    {
        pool->queue_head = conn;
    }
    pool->queue_tail = conn;
    pool->queue_size++;
    pool->total_connections++;

    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
}

/**
 * Worker thread function
 */
static void *worker_thread(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    while (1)
    {
        pthread_mutex_lock(&pool->queue_mutex);

        /* Wait for connections */
        while (!pool->queue_head && !pool->shutdown)
        {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }

        if (pool->shutdown)
        {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        /* Get connection from queue */
        Connection *conn = pool->queue_head;
        pool->queue_head = conn->next;
        if (!pool->queue_head)
        {
            pool->queue_tail = NULL;
        }
        pool->queue_size--;
        pool->active_connections++;

        pthread_mutex_unlock(&pool->queue_mutex);

        /* Handle connection with keep-alive support */
        handle_connection(conn->fd, conn->doc_root);

        /* Update stats */
        pthread_mutex_lock(&pool->queue_mutex);
        pool->active_connections--;
        pthread_mutex_unlock(&pool->queue_mutex);

        free(conn);
    }

    return NULL;
}

/**
 * Handle connection with keep-alive support
 */
static void handle_connection(int fd, const char *doc_root)
{
    bool keep_alive = true;
    int requests = 0;

    while (keep_alive && requests < kKeepAliveMax)
    {
        if (process_request(fd, doc_root, &keep_alive) < 0)
        {
            break;
        }
        requests++;

        /* Update global stats */
        if (g_pool)
        {
            __atomic_fetch_add(&g_pool->total_requests, 1, __ATOMIC_RELAXED);
        }

        if (!keep_alive)
            break;

        /* Set timeout for next request */
        struct timeval tv = {
            .tv_sec = kKeepAliveTimeout,
            .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    close(fd);
}

/**
 * Process a single HTTP request
 */
static int process_request(int fd, const char *doc_root, bool *keep_alive)
{
    char request_buffer[kMaxRequestSize];
    char file_path[kMaxPathSize];

    /* Read request */
    ssize_t bytes_read = recv(fd, request_buffer, sizeof(request_buffer) - 1, 0);
    if (bytes_read <= 0)
    {
        return -1;
    }
    request_buffer[bytes_read] = '\0';

    /* Parse request */
    http_req_t request;
    if (http_parse_request(request_buffer, bytes_read, &request) <= 0)
    {
        send_error_response(fd, 400, false);
        *keep_alive = false;
        return -1;
    }

    /* Check for keep-alive */
    *keep_alive = (strstr(request_buffer, "Connection: keep-alive") != NULL ||
                   strstr(request_buffer, "HTTP/1.1") != NULL);

    /* Build file path */
    if (http_safe_join(file_path, sizeof(file_path), doc_root, request.path) < 0)
    {
        send_error_response(fd, 404, *keep_alive);
        return 0;
    }

    /* Check file */
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode))
    {
        send_error_response(fd, 404, *keep_alive);
        return 0;
    }

    /* Send response */
    return send_file_response(fd, file_path, *keep_alive);
}

/**
 * Send file response with keep-alive support
 */
static int send_file_response(int client_fd, const char *file_path, bool keep_alive)
{
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0)
    {
        return send_error_response(client_fd, 500, keep_alive);
    }

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) != 0)
    {
        close(file_fd);
        return send_error_response(client_fd, 500, keep_alive);
    }

    /* Build header with keep-alive */
    char header[kMaxHeaderSize];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Length: %lld\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              (long long)file_stat.st_size,
                              http_guess_type(file_path),
                              keep_alive ? "keep-alive" : "close");

    if (send(client_fd, header, header_len, MSG_NOSIGNAL) < 0)
    {
        close(file_fd);
        return -1;
    }

    /* Send file using sendfile or read/write */
    char buffer[kFileBufferSize];
    ssize_t bytes_read;

    while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0)
    {
        ssize_t total_sent = 0;
        while (total_sent < bytes_read)
        {
            ssize_t sent = send(client_fd, buffer + total_sent,
                                bytes_read - total_sent, MSG_NOSIGNAL);
            if (sent <= 0)
            {
                close(file_fd);
                return -1;
            }
            total_sent += sent;
        }
    }

    close(file_fd);
    return 0;
}

/**
 * Send error response with keep-alive support
 */
static int send_error_response(int client_fd, int status_code, bool keep_alive)
{
    const char *status_text;
    const char *body;

    switch (status_code)
    {
    case 400:
        status_text = "400 Bad Request";
        body = "Bad Request";
        break;
    case 404:
        status_text = "404 Not Found";
        body = "Not Found";
        break;
    case 500:
        status_text = "500 Internal Server Error";
        body = "Internal Server Error";
        break;
    default:
        return -1;
    }

    char response[kMaxHeaderSize];
    int response_len = snprintf(response, sizeof(response),
                                "HTTP/1.1 %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Content-Type: text/plain\r\n"
                                "Connection: %s\r\n"
                                "\r\n"
                                "%s",
                                status_text,
                                strlen(body),
                                keep_alive ? "keep-alive" : "close",
                                body);

    return send(client_fd, response, response_len, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

/**
 * Create server socket optimized for C10K
 */
static int create_server_socket(const char *bind_addr, int port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return -1;
    }

    /* Enable address reuse */
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    /* Enable port reuse for load balancing */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0)
    {
        /* Non-fatal */
    }
#endif

    /* Bind */
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = bind_addr ? inet_addr(bind_addr) : INADDR_ANY};

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }

    /* Listen with large backlog */
    if (listen(server_fd, kListenBacklog) < 0)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

/**
 * Configure socket for performance
 */
static void configure_socket_options(int socket_fd)
{
    /* TCP no delay */
    int nodelay = 1;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Socket timeouts */
    struct timeval timeout = {
        .tv_sec = kSocketTimeoutSec,
        .tv_usec = 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* Socket buffer sizes */
    int bufsize = 65536;
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
}

/**
 * Increase system limits for C10K
 */
static int increase_limits(void)
{
    struct rlimit rlim;

    /* Increase file descriptor limit */
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0)
    {
        rlim.rlim_cur = rlim.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rlim) < 0)
        {
            rlim.rlim_cur = 65536;
            rlim.rlim_max = 65536;
            setrlimit(RLIMIT_NOFILE, &rlim);
        }
    }

    /* Increase thread limit (Linux only) */
#ifdef RLIMIT_NPROC
    if (getrlimit(RLIMIT_NPROC, &rlim) == 0)
    {
        rlim.rlim_cur = 10000;
        rlim.rlim_max = 10000;
        setrlimit(RLIMIT_NPROC, &rlim);
    }
#endif

    return 0;
}