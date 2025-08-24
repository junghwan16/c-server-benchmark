/**
 * Kqueue-based HTTP Server Implementation
 *
 * Design goals:
 * - Handle C10K+ concurrent connections
 * - Efficient event-driven I/O using kqueue
 * - Zero-copy where possible
 * - Memory efficient connection management
 */

#include "kqueue_server.h"
#include "../common/http.h"
#include "../common/util.h"

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>

/* Configuration */
enum
{
    kMaxEvents = 1024,           /* Events to process per iteration - increased */
    kMaxConnections = 50000,     /* Support up to 50K connections */
    kRequestBufferSize = 4096,   /* HTTP request buffer - reduced */
    kResponseBufferSize = 32768, /* Response chunk size - reduced */
    kPathBufferSize = 1024,      /* File path buffer - reduced */
    kHeaderBufferSize = 512,     /* HTTP header buffer */
    kListenBacklog = 10000,      /* Listen queue size - match somaxconn */
};

/* Connection states */
typedef enum
{
    STATE_READING_REQUEST,
    STATE_PROCESSING,
    STATE_SENDING_HEADER,
    STATE_SENDING_FILE,
    STATE_CLOSING
} ConnectionState;

/* Connection structure */
typedef struct Connection
{
    int fd;
    ConnectionState state;

    /* Request handling */
    char *request_buffer;
    size_t request_size;
    size_t request_capacity;

    /* Response handling */
    char *response_buffer;
    size_t response_size;
    size_t response_sent;

    /* File serving */
    int file_fd;
    off_t file_offset;
    off_t file_size;

    /* For connection pool */
    struct Connection *next;

    /* Server context */
    struct Server *server;
} Connection;

/* Server context */
typedef struct Server
{
    int kq;               /* Kqueue descriptor */
    int listen_fd;        /* Listening socket */
    const char *doc_root; /* Document root */

    /* Connection pool */
    Connection *connections; /* Array of all connections */
    Connection *free_list;   /* Free connection list */
    int num_active;          /* Active connections count */

    /* Statistics */
    uint64_t total_requests;
    uint64_t total_bytes_sent;
    uint64_t total_connections;
} Server;

/* Function prototypes */
static int increase_fd_limit(void);
static int create_listen_socket(const char *bind_addr, int port);
static Connection *alloc_connection(Server *server);
static void free_connection(Server *server, Connection *conn);
static void reset_connection(Connection *conn);
static void close_connection(Server *server, Connection *conn);
static int accept_connections(Server *server);
static int handle_read_event(Server *server, Connection *conn);
static int handle_write_event(Server *server, Connection *conn);
static int process_request(Server *server, Connection *conn);
static int prepare_file_response(Connection *conn, const char *file_path);
static int prepare_error_response(Connection *conn, int status_code);
static int send_response(Connection *conn);

/**
 * Main server entry point
 */
int run_kqueue_server(const char *bind_addr, int port, const char *doc_root)
{
    if (!doc_root)
    {
        fprintf(stderr, "Error: document root required\n");
        return -1;
    }

    /* Increase file descriptor limit for C10K+ */
    if (increase_fd_limit() < 0)
    {
        fprintf(stderr, "Warning: Could not increase fd limit\n");
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Initialize server */
    Server server = {0};
    server.doc_root = doc_root;

    /* Create kqueue */
    server.kq = kqueue();
    if (server.kq < 0)
    {
        perror("kqueue");
        return -1;
    }

    /* Create listening socket */
    server.listen_fd = create_listen_socket(bind_addr, port);
    if (server.listen_fd < 0)
    {
        close(server.kq);
        return -1;
    }

    /* Initialize connection pool */
    server.connections = calloc(kMaxConnections, sizeof(Connection));
    if (!server.connections)
    {
        perror("calloc");
        close(server.listen_fd);
        close(server.kq);
        return -1;
    }

    /* Build free list */
    for (int i = 0; i < kMaxConnections - 1; i++)
    {
        server.connections[i].next = &server.connections[i + 1];
        server.connections[i].fd = -1;
    }
    server.connections[kMaxConnections - 1].fd = -1;
    server.free_list = &server.connections[0];

    /* Register listen socket with kqueue */
    struct kevent ev;
    EV_SET(&ev, server.listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(server.kq, &ev, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent");
        free(server.connections);
        close(server.listen_fd);
        close(server.kq);
        return -1;
    }

    fprintf(stderr, "Kqueue server listening on %s:%d (doc_root: %s)\n",
            bind_addr ? bind_addr : "0.0.0.0", port, doc_root);
    fprintf(stderr, "Max connections: %d\n", kMaxConnections);

    /* Event loop */
    struct kevent events[kMaxEvents];
    int running = 1;

    while (running)
    {
        int nev = kevent(server.kq, NULL, 0, events, kMaxEvents, NULL);

        if (nev < 0)
        {
            if (errno == EINTR)
                continue;
            perror("kevent");
            break;
        }

        /* Process events */
        for (int i = 0; i < nev; i++)
        {
            struct kevent *ev = &events[i];

            if (ev->flags & EV_ERROR)
            {
                fprintf(stderr, "EV_ERROR: %s\n", strerror(ev->data));
                continue;
            }

            if (ev->ident == (uintptr_t)server.listen_fd)
            {
                /* New connection */
                accept_connections(&server);
            }
            else
            {
                /* Client I/O */
                Connection *conn = (Connection *)ev->udata;
                if (!conn)
                    continue;

                if (ev->filter == EVFILT_READ)
                {
                    if (handle_read_event(&server, conn) < 0)
                    {
                        close_connection(&server, conn);
                    }
                }
                else if (ev->filter == EVFILT_WRITE)
                {
                    if (handle_write_event(&server, conn) < 0)
                    {
                        close_connection(&server, conn);
                    }
                }
            }
        }

        /* Print stats periodically */
        static time_t last_stats = 0;
        static int max_active = 0;
        time_t now = time(NULL);

        if (server.num_active > max_active)
            max_active = server.num_active;

        if (now - last_stats >= 10)
        {
            fprintf(stderr, "Stats: active=%d max=%d total=%llu requests=%llu bytes=%llu\n",
                    server.num_active,
                    max_active,
                    (unsigned long long)server.total_connections,
                    (unsigned long long)server.total_requests,
                    (unsigned long long)server.total_bytes_sent);
            last_stats = now;
        }
    }

    /* Cleanup */
    for (int i = 0; i < kMaxConnections; i++)
    {
        if (server.connections[i].fd >= 0)
        {
            close(server.connections[i].fd);
        }
        free(server.connections[i].request_buffer);
        free(server.connections[i].response_buffer);
    }
    free(server.connections);
    close(server.listen_fd);
    close(server.kq);

    return 0;
}

/**
 * Increase file descriptor limit for C10K+
 */
static int increase_fd_limit(void)
{
    struct rlimit rlim;

    /* Get current limits */
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
    {
        return -1;
    }

    /* Try to set to maximum */
    rlim.rlim_cur = rlim.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rlim) < 0)
    {
        /* Try a reasonable value */
        rlim.rlim_cur = 65536;
        rlim.rlim_max = 65536;
        if (setrlimit(RLIMIT_NOFILE, &rlim) < 0)
        {
            return -1;
        }
    }

    fprintf(stderr, "File descriptor limit: %llu\n",
            (unsigned long long)rlim.rlim_cur);
    return 0;
}

/**
 * Create and configure listening socket
 */
static int create_listen_socket(const char *bind_addr, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    /* Set non-blocking */
    if (set_nonblock(fd) < 0)
    {
        close(fd);
        return -1;
    }

    /* Allow address reuse */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("SO_REUSEADDR");
        close(fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    /* Allow port reuse (load balancing) */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
    {
        /* Non-fatal */
    }
#endif

    /* Bind */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = bind_addr ? inet_addr(bind_addr) : INADDR_ANY};

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Listen */
    if (listen(fd, kListenBacklog) < 0)
    {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * Allocate connection from pool
 */
static Connection *alloc_connection(Server *server)
{
    if (!server->free_list)
    {
        return NULL; /* Pool exhausted */
    }

    Connection *conn = server->free_list;
    server->free_list = conn->next;
    conn->next = NULL;

    reset_connection(conn);
    server->num_active++;
    server->total_connections++;

    return conn;
}

/**
 * Return connection to pool
 */
static void free_connection(Server *server, Connection *conn)
{
    if (conn->fd >= 0)
    {
        close(conn->fd);
        conn->fd = -1;
    }

    if (conn->file_fd >= 0)
    {
        close(conn->file_fd);
        conn->file_fd = -1;
    }

    conn->next = server->free_list;
    server->free_list = conn;
    server->num_active--;
}

/**
 * Reset connection state
 */
static void reset_connection(Connection *conn)
{
    conn->state = STATE_READING_REQUEST;
    conn->request_size = 0;
    conn->response_size = 0;
    conn->response_sent = 0;
    conn->file_fd = -1;
    conn->file_offset = 0;
    conn->file_size = 0;
}

/**
 * Close connection and cleanup
 */
static void close_connection(Server *server, Connection *conn)
{
    /* Remove from kqueue */
    if (conn->fd >= 0)
    {
        struct kevent ev[2];
        EV_SET(&ev[0], conn->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&ev[1], conn->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(server->kq, ev, 2, NULL, 0, NULL);
    }

    free_connection(server, conn);
}

/**
 * Accept new connections
 */
static int accept_connections(Server *server)
{
    while (1)
    {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);

        int fd = accept(server->listen_fd, (struct sockaddr *)&addr, &addrlen);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; /* No more connections */
            }
            perror("accept");
            break;
        }

        /* Get connection from pool */
        Connection *conn = alloc_connection(server);
        if (!conn)
        {
            close(fd); /* Pool exhausted */
            continue;
        }

        /* Configure socket */
        set_nonblock(fd);
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* Initialize connection */
        conn->fd = fd;
        conn->server = server;

        /* Allocate buffers on demand - lazy allocation saves memory */
        if (!conn->request_buffer)
        {
            conn->request_buffer = malloc(kRequestBufferSize);
            if (!conn->request_buffer)
            {
                close(fd);
                free_connection(server, conn);
                continue;
            }
            conn->request_capacity = kRequestBufferSize;
        }

        /* Register with kqueue */
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, conn);
        if (kevent(server->kq, &ev, 1, NULL, 0, NULL) < 0)
        {
            perror("kevent");
            close_connection(server, conn);
        }
    }

    return 0;
}

/**
 * Handle read events
 */
static int handle_read_event(Server *server, Connection *conn)
{
    if (conn->state != STATE_READING_REQUEST)
    {
        return 0;
    }

    /* Read request */
    ssize_t n = recv(conn->fd,
                     conn->request_buffer + conn->request_size,
                     conn->request_capacity - conn->request_size - 1, 0);

    if (n <= 0)
    {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return 0; /* Try again later */
        }
        return -1; /* Connection closed or error */
    }

    conn->request_size += n;
    conn->request_buffer[conn->request_size] = '\0';

    /* Check if request is complete */
    if (strstr(conn->request_buffer, "\r\n\r\n"))
    {
        return process_request(server, conn);
    }

    /* Check buffer overflow */
    if (conn->request_size >= conn->request_capacity - 1)
    {
        prepare_error_response(conn, 413); /* Request Too Large */
        return 0;
    }

    return 0;
}

/**
 * Handle write events
 */
static int handle_write_event(Server *server, Connection *conn)
{
    (void)server; /* Unused */

    if (conn->state == STATE_SENDING_HEADER || conn->state == STATE_SENDING_FILE)
    {
        return send_response(conn);
    }

    return 0;
}

/**
 * Process HTTP request
 */
static int process_request(Server *server, Connection *conn)
{
    http_req_t request;
    char file_path[kPathBufferSize];

    /* Parse request */
    if (http_parse_request(conn->request_buffer, conn->request_size, &request) <= 0)
    {
        prepare_error_response(conn, 400); /* Bad Request */
        return 0;
    }

    /* Build file path */
    if (http_safe_join(file_path, sizeof(file_path),
                       server->doc_root, request.path) < 0)
    {
        prepare_error_response(conn, 404); /* Not Found */
        return 0;
    }

    /* Check file */
    struct stat st;
    if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode))
    {
        prepare_error_response(conn, 404); /* Not Found */
        return 0;
    }

    /* Prepare response */
    if (prepare_file_response(conn, file_path) < 0)
    {
        prepare_error_response(conn, 500); /* Internal Server Error */
        return 0;
    }

    server->total_requests++;
    return 0;
}

/**
 * Prepare file response
 */
static int prepare_file_response(Connection *conn, const char *file_path)
{
    /* Open file */
    conn->file_fd = open(file_path, O_RDONLY);
    if (conn->file_fd < 0)
    {
        return -1;
    }

    /* Get file size */
    struct stat st;
    if (fstat(conn->file_fd, &st) < 0)
    {
        close(conn->file_fd);
        conn->file_fd = -1;
        return -1;
    }

    conn->file_size = st.st_size;
    conn->file_offset = 0;

    /* Build header */
    char header[kHeaderBufferSize];
    int header_len = http_build_200(header, sizeof(header),
                                    st.st_size, http_guess_type(file_path));
    if (header_len < 0)
    {
        close(conn->file_fd);
        conn->file_fd = -1;
        return -1;
    }

    /* Allocate response buffer if needed */
    if (!conn->response_buffer)
    {
        conn->response_buffer = malloc(kResponseBufferSize);
        if (!conn->response_buffer)
        {
            close(conn->file_fd);
            conn->file_fd = -1;
            return -1;
        }
    }

    /* Copy header to response buffer */
    memcpy(conn->response_buffer, header, header_len);
    conn->response_size = header_len;
    conn->response_sent = 0;
    conn->state = STATE_SENDING_HEADER;

    /* Enable write events */
    struct kevent ev;
    EV_SET(&ev, conn->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, conn);
    if (kevent(conn->server->kq, &ev, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent write enable");
        return -1;
    }

    return 0;
}

/**
 * Prepare error response
 */
static int prepare_error_response(Connection *conn, int status_code)
{
    char response[kHeaderBufferSize];
    int response_len = 0;

    switch (status_code)
    {
    case 400:
        response_len = snprintf(response, sizeof(response),
                                "HTTP/1.1 400 Bad Request\r\n"
                                "Content-Length: 11\r\n"
                                "Connection: close\r\n\r\n"
                                "Bad Request");
        break;

    case 404:
        response_len = http_build_404(response, sizeof(response));
        break;

    case 413:
        response_len = snprintf(response, sizeof(response),
                                "HTTP/1.1 413 Request Entity Too Large\r\n"
                                "Content-Length: 18\r\n"
                                "Connection: close\r\n\r\n"
                                "Request Too Large");
        break;

    case 500:
        response_len = snprintf(response, sizeof(response),
                                "HTTP/1.1 500 Internal Server Error\r\n"
                                "Content-Length: 21\r\n"
                                "Connection: close\r\n\r\n"
                                "Internal Server Error");
        break;

    default:
        return -1;
    }

    if (response_len <= 0 || (size_t)response_len >= sizeof(response))
    {
        return -1;
    }

    /* Allocate buffer if needed */
    if (!conn->response_buffer)
    {
        conn->response_buffer = malloc(kResponseBufferSize);
        if (!conn->response_buffer)
        {
            return -1;
        }
    }

    /* Copy response */
    memcpy(conn->response_buffer, response, response_len);
    conn->response_size = response_len;
    conn->response_sent = 0;
    conn->state = STATE_SENDING_HEADER;

    /* Enable write events */
    struct kevent ev;
    EV_SET(&ev, conn->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, conn);
    if (kevent(conn->server->kq, &ev, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent write enable");
        return -1;
    }

    return 0;
}

/**
 * Send response data
 */
static int send_response(Connection *conn)
{

    /* Send header if needed */
    if (conn->state == STATE_SENDING_HEADER)
    {
        ssize_t n = send(conn->fd,
                         conn->response_buffer + conn->response_sent,
                         conn->response_size - conn->response_sent,
                         MSG_NOSIGNAL);

        if (n <= 0)
        {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return 0; /* Try again later */
            }
            return -1; /* Error */
        }

        conn->response_sent += n;

        /* Check if header sent */
        if (conn->response_sent >= conn->response_size)
        {
            if (conn->file_fd >= 0)
            {
                conn->state = STATE_SENDING_FILE;
                conn->response_sent = 0;
            }
            else
            {
                return -1; /* Done, close connection */
            }
        }
    }

    /* Send file content */
    if (conn->state == STATE_SENDING_FILE && conn->file_fd >= 0)
    {
        /* Read chunk from file */
        ssize_t to_read = kResponseBufferSize;
        if (conn->file_offset + to_read > conn->file_size)
        {
            to_read = conn->file_size - conn->file_offset;
        }

        if (to_read == 0)
        {
            return -1; /* Done */
        }

        ssize_t n = pread(conn->file_fd, conn->response_buffer,
                          to_read, conn->file_offset);
        if (n <= 0)
        {
            return -1; /* Error */
        }

        /* Send chunk */
        ssize_t sent = send(conn->fd, conn->response_buffer, n, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return 0; /* Try again later */
            }
            return -1; /* Error */
        }

        conn->file_offset += sent;

        /* Update stats if we had server pointer */
        /* server->total_bytes_sent += sent; */

        /* Check if done */
        if (conn->file_offset >= conn->file_size)
        {
            return -1; /* Done, close connection */
        }
    }

    return 0;
}