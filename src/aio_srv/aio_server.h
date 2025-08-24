#ifndef AIO_SERVER_H
#define AIO_SERVER_H

/**
 * Asynchronous I/O HTTP Server
 * 
 * Event-driven server using select() for multiplexed I/O.
 * Handles multiple concurrent connections without threads.
 */

/**
 * Starts the async I/O HTTP server
 * 
 * @param bind_addr IP address to bind (NULL for INADDR_ANY)
 * @param port      Port number to listen on
 * @param doc_root  Document root directory path
 * @return          0 on success, -1 on failure
 */
int run_aio_server(const char *bind_addr, int port, const char *doc_root);

#endif /* AIO_SERVER_H */