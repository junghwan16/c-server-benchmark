#ifndef THREAD_SERVER_H
#define THREAD_SERVER_H

/**
 * Starts the thread-based HTTP server
 *
 * @param bind_addr IP address to bind (NULL for INADDR_ANY)
 * @param port      Port number to listen on
 * @param doc_root  Document root directory path
 * @return          0 on success, -1 on failure
 */
int run_thread_server(const char *bind_addr, int port, const char *doc_root);

#endif /* THREAD_SERVER_H */