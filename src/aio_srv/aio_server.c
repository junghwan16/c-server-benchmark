/**
 * Asynchronous I/O HTTP Server Implementation
 *
 * Design philosophy:
 * - Simple state machine per connection
 * - Non-blocking I/O with select()
 */

#include "aio_server.h"
#include "../common/http.h"
#include "../common/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Configuration constants */
enum
{
  kMaxClients = 100,            /* Limited for stack allocation */
  kRequestBufferSize = 8192,    /* Larger for modern HTTP */
  kResponseBufferSize = 65536,  /* 64KB for optimal I/O */
  kPathBufferSize = 2048,       /* PATH_MAX compatible */
  kHeaderBufferSize = 512,      /* Response header size */
  kListenBacklog = 512,         /* Higher for production */
  kSelectTimeoutMs = 50         /* Lower latency checks */
};

/* Client connection states */
typedef enum
{
  STATE_READING_REQUEST,
  STATE_SENDING_RESPONSE,
  STATE_CLOSING
} ClientState;

/* Client connection structure */
typedef struct
{
  int fd;
  ClientState state;

  /* Request handling */
  char request_buffer[kRequestBufferSize];
  size_t request_size;

  /* Response handling */
  char *response_buffer;
  size_t response_size;
  size_t response_sent;

  /* File serving */
  int file_fd;
  off_t file_offset;
  off_t file_size;
} Client;

/* Server context */
typedef struct
{
  int listen_fd;
  const char *doc_root;
  Client clients[kMaxClients];
  int num_clients;

  /* Statistics */
  unsigned long total_requests;
  unsigned long total_bytes_sent;
} Server;

/* Function prototypes */
static int create_listen_socket(const char *bind_addr, int port);
static void accept_new_clients(Server *server);
static void handle_client_read(Server *server, Client *client);
static void handle_client_write(Server *server, Client *client);
static void process_http_request(Server *server, Client *client);
static void prepare_file_response(Client *client, const char *file_path);
static void prepare_error_response(Client *client, int status_code);
static void close_client(Client *client);
static void reset_client(Client *client);

/**
 * Main server entry point
 */
int run_aio_server(const char *bind_addr, int port, const char *doc_root)
{
  if (!doc_root)
  {
    fprintf(stderr, "Error: document root is required\n");
    return -1;
  }

  /* Ignore SIGPIPE */
  signal(SIGPIPE, SIG_IGN);

  /* Initialize server context - allocate on heap to avoid stack overflow */
  Server *server = calloc(1, sizeof(Server));
  if (!server) {
    fprintf(stderr, "Failed to allocate server context\n");
    return -1;
  }
  server->doc_root = doc_root;

  /* Create listening socket */
  server->listen_fd = create_listen_socket(bind_addr, port);
  if (server->listen_fd < 0)
  {
    free(server);
    return -1;
  }

  /* Initialize client slots */
  for (int i = 0; i < kMaxClients; i++)
  {
    reset_client(&server->clients[i]);
  }

  fprintf(stderr, "AIO server listening on %s:%d (doc_root: %s)\n",
          bind_addr ? bind_addr : "0.0.0.0", port, doc_root);

  /* Main event loop */
  while (1)
  {
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    /* Add listen socket */
    FD_SET(server->listen_fd, &read_fds);
    int max_fd = server->listen_fd;

    /* Add client sockets */
    for (int i = 0; i < kMaxClients; i++)
    {
      Client *client = &server->clients[i];
      if (client->fd < 0)
        continue;

      if (client->state == STATE_READING_REQUEST)
      {
        FD_SET(client->fd, &read_fds);
      }
      else if (client->state == STATE_SENDING_RESPONSE)
      {
        FD_SET(client->fd, &write_fds);
      }

      if (client->fd > max_fd)
      {
        max_fd = client->fd;
      }
    }

    /* Wait for events */
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = kSelectTimeoutMs * 1000};

    int ready = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    /* Handle new connections */
    if (FD_ISSET(server->listen_fd, &read_fds))
    {
      accept_new_clients(server);
    }

    /* Handle client I/O */
    for (int i = 0; i < kMaxClients; i++)
    {
      Client *client = &server->clients[i];
      if (client->fd < 0)
        continue;

      if (FD_ISSET(client->fd, &read_fds))
      {
        handle_client_read(server, client);
      }

      if (client->fd >= 0 && FD_ISSET(client->fd, &write_fds))
      {
        handle_client_write(server, client);
      }
    }

    /* Print statistics periodically */
    static int counter = 0;
    if (++counter % 1000 == 0)
    {
      fprintf(stderr, "Stats: clients=%d requests=%lu bytes_sent=%lu\n",
              server->num_clients, server->total_requests, server->total_bytes_sent);
    }
  }

  /* Cleanup */
  close(server->listen_fd);
  for (int i = 0; i < kMaxClients; i++)
  {
    if (server->clients[i].fd >= 0)
    {
      close_client(&server->clients[i]);
    }
  }
  free(server);

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

  /* Enable address reuse */
  int reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
  {
    perror("setsockopt(SO_REUSEADDR)");
    close(fd);
    return -1;
  }

  /* Set non-blocking mode */
  if (set_nonblock(fd) < 0)
  {
    perror("set_nonblock");
    close(fd);
    return -1;
  }

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
 * Accept new client connections
 */
static void accept_new_clients(Server *server)
{
  while (1)
  {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(server->listen_fd,
                           (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        break; /* No more connections pending */
      }
      perror("accept");
      break;
    }

    /* Find free client slot */
    Client *client = NULL;
    for (int i = 0; i < kMaxClients; i++)
    {
      if (server->clients[i].fd < 0)
      {
        client = &server->clients[i];
        break;
      }
    }

    if (!client)
    {
      fprintf(stderr, "Server full, rejecting connection\n");
      close(client_fd);
      continue;
    }

    /* Configure client socket */
    set_nonblock(client_fd);
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Initialize client */
    reset_client(client);
    client->fd = client_fd;
    client->state = STATE_READING_REQUEST;
    server->num_clients++;
  }
}

/**
 * Handle client read events
 */
static void handle_client_read(Server *server, Client *client)
{
  ssize_t n = recv(client->fd,
                   client->request_buffer + client->request_size,
                   sizeof(client->request_buffer) - client->request_size - 1, 0);

  if (n <= 0)
  {
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      return; /* Try again later */
    }
    close_client(client);
    server->num_clients--;
    return;
  }

  client->request_size += n;
  client->request_buffer[client->request_size] = '\0';

  /* Check if request is complete */
  if (strstr(client->request_buffer, "\r\n\r\n"))
  {
    process_http_request(server, client);
    server->total_requests++;
  }
}

/**
 * Handle client write events
 */
static void handle_client_write(Server *server, Client *client)
{
  /* Send from response buffer */
  if (client->response_buffer)
  {
    ssize_t n = send(client->fd,
                     client->response_buffer + client->response_sent,
                     client->response_size - client->response_sent,
                     MSG_NOSIGNAL);

    if (n <= 0)
    {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        return; /* Try again later */
      }
      close_client(client);
      server->num_clients--;
      return;
    }

    client->response_sent += n;
    server->total_bytes_sent += n;

    /* Check if response complete */
    if (client->response_sent >= client->response_size)
    {
      free(client->response_buffer);
      client->response_buffer = NULL;

      /* If we have a file to send, continue with that */
      if (client->file_fd >= 0)
      {
        client->state = STATE_SENDING_RESPONSE;
      }
      else
      {
        close_client(client);
        server->num_clients--;
      }
    }
    return;
  }

  /* Send file content */
  if (client->file_fd >= 0)
  {
    char buffer[kResponseBufferSize];
    ssize_t to_read = sizeof(buffer);
    if (client->file_offset + to_read > client->file_size)
    {
      to_read = client->file_size - client->file_offset;
    }

    if (to_read == 0)
    {
      close_client(client);
      server->num_clients--;
      return;
    }

    ssize_t n = pread(client->file_fd, buffer, to_read, client->file_offset);
    if (n <= 0)
    {
      close_client(client);
      server->num_clients--;
      return;
    }

    ssize_t sent = send(client->fd, buffer, n, MSG_NOSIGNAL);
    if (sent <= 0)
    {
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        return; /* Try again later */
      }
      close_client(client);
      server->num_clients--;
      return;
    }

    client->file_offset += sent;
    server->total_bytes_sent += sent;

    /* Check if file transfer complete */
    if (client->file_offset >= client->file_size)
    {
      close_client(client);
      server->num_clients--;
    }
  }
}

/**
 * Process HTTP request and prepare response
 */
static void process_http_request(Server *server, Client *client)
{
  http_req_t request;
  char file_path[kPathBufferSize];

  /* Parse request */
  if (http_parse_request(client->request_buffer,
                         client->request_size, &request) <= 0)
  {
    prepare_error_response(client, 400); /* Bad Request */
    return;
  }

  /* Build safe file path */
  if (http_safe_join(file_path, sizeof(file_path),
                     server->doc_root, request.path) < 0)
  {
    prepare_error_response(client, 404); /* Not Found */
    return;
  }

  /* Check file */
  struct stat st;
  if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode))
  {
    prepare_error_response(client, 404); /* Not Found */
    return;
  }

  /* Prepare file response */
  prepare_file_response(client, file_path);
}

/**
 * Prepare file response
 */
static void prepare_file_response(Client *client, const char *file_path)
{
  /* Open file */
  client->file_fd = open(file_path, O_RDONLY);
  if (client->file_fd < 0)
  {
    prepare_error_response(client, 500); /* Internal Server Error */
    return;
  }

  /* Get file info */
  struct stat st;
  if (fstat(client->file_fd, &st) < 0)
  {
    close(client->file_fd);
    client->file_fd = -1;
    prepare_error_response(client, 500);
    return;
  }

  client->file_size = st.st_size;
  client->file_offset = 0;

  /* Build HTTP header */
  char header[kHeaderBufferSize];
  int header_len = http_build_200(header, sizeof(header),
                                  st.st_size, http_guess_type(file_path));
  if (header_len < 0)
  {
    close(client->file_fd);
    client->file_fd = -1;
    prepare_error_response(client, 500);
    return;
  }

  /* Allocate response buffer for header */
  client->response_buffer = malloc(header_len + 1); /* +1 for safety */
  if (!client->response_buffer)
  {
    close(client->file_fd);
    client->file_fd = -1;
    prepare_error_response(client, 500);
    return;
  }

  memcpy(client->response_buffer, header, header_len);
  client->response_size = header_len;
  client->response_sent = 0;
  client->state = STATE_SENDING_RESPONSE;
}

/**
 * Prepare error response
 */
static void prepare_error_response(Client *client, int status_code)
{
  char response[kHeaderBufferSize];
  int response_len = 0;

  switch (status_code)
  {
  case 400:
    response_len = snprintf(response, sizeof(response),
                            "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Length: 11\r\n"
                            "Content-Type: text/plain\r\n"
                            "Connection: close\r\n\r\n"
                            "Bad Request");
    break;

  case 404:
    response_len = http_build_404(response, sizeof(response));
    break;

  case 500:
    response_len = snprintf(response, sizeof(response),
                            "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Length: 21\r\n"
                            "Content-Type: text/plain\r\n"
                            "Connection: close\r\n\r\n"
                            "Internal Server Error");
    break;

  default:
    response_len = snprintf(response, sizeof(response),
                            "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Length: 5\r\n"
                            "Connection: close\r\n\r\n"
                            "Error");
    break;
  }

  if (response_len <= 0 || (size_t)response_len >= sizeof(response))
  {
    close_client(client);
    return;
  }

  /* Allocate response buffer */
  client->response_buffer = malloc(response_len + 1); /* +1 for safety */
  if (!client->response_buffer)
  {
    close_client(client);
    return;
  }

  memcpy(client->response_buffer, response, response_len);
  client->response_size = response_len;
  client->response_sent = 0;
  client->state = STATE_SENDING_RESPONSE;
}

/**
 * Close client connection and free resources
 */
static void close_client(Client *client)
{
  if (client->fd >= 0)
  {
    close(client->fd);
    client->fd = -1;
  }

  if (client->file_fd >= 0)
  {
    close(client->file_fd);
    client->file_fd = -1;
  }

  if (client->response_buffer)
  {
    free(client->response_buffer);
    client->response_buffer = NULL;
  }

  reset_client(client);
}

/**
 * Reset client structure to initial state
 */
static void reset_client(Client *client)
{
  client->fd = -1;
  client->state = STATE_READING_REQUEST;
  client->request_size = 0;
  client->response_buffer = NULL;
  client->response_size = 0;
  client->response_sent = 0;
  client->file_fd = -1;
  client->file_offset = 0;
  client->file_size = 0;
  memset(client->request_buffer, 0, sizeof(client->request_buffer));
}