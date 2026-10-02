#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Returns a listening socket, or -1 on error. Caller closes the socket. */
int tcp_open_listener(int port);

/* Accept clients and dispatch workers; does not close serv_sock. */
void tcp_accept_loop(int serv_sock);

ssize_t write_all(int fd, const void *buf, size_t total);
int8_t send_file_str_len(int file_str_len, int cli_sock);
int8_t send_file_list(const char files[], int file_str_len, int cli_sock);
int8_t send_initial_files_info(const char files[], int file_str_len,
                             int cli_sock);

#endif
