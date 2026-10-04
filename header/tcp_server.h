#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>

/* Returns a listening socket, or -1 on error. Caller closes the socket. */
int tcp_open_listener(int port);

/* Start the TCP control thread and return immediately.
 * Returns 0 on success or an error number on failure.
 * On success, the caller signals stop_evt_fd and joins *out_thread before
 * closing either fd. out_thread is only valid after a successful call. */
int tcp_accept_loop(int serv_sock, int stop_evt_fd, pthread_t *out_thread);

ssize_t write_all(int fd, const void *buf, size_t total);
int8_t send_file_str_len(int file_str_len, int cli_sock);
int8_t send_file_list(const char files[], int file_str_len, int cli_sock);
int8_t send_initial_files_info(const char files[], int file_str_len,
                             int cli_sock);

#endif
