#ifndef KQUEUE_SERVER_H
#define KQUEUE_SERVER_H

/**
 * Kqueue-based HTTP Server (BSD/macOS)
 * 
 * High-performance event-driven server using kqueue.
 * Designed to handle C10K+ concurrent connections efficiently.
 */

/**
 * Starts the kqueue-based HTTP server
 * 
 * @param bind_addr IP address to bind (NULL for INADDR_ANY)
 * @param port      Port number to listen on
 * @param doc_root  Document root directory path
 * @return          0 on success, -1 on failure
 */
int run_kqueue_server(const char *bind_addr, int port, const char *doc_root);

#endif /* KQUEUE_SERVER_H */