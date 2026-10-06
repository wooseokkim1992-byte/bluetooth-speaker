#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#define CLIENT_BUCKET_COUNT 10
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include "tcp_interface.h"

typedef enum _RECEOVED_STATE
{
    NOTHING,
    HEADER,
    PAYLOAD
} RECEOVED_STATE;

typedef enum _SESSION_STATE
{
    INIT,
    PLAYING,
    PAUSED,
    STOPPING
} SESSION_STATE;

typedef struct
{
    int fd;
    uint8_t client_id;    // 1바이트 바이너리 UUID
    SESSION_STATE status; // PLAYING, STOPPING, PAUSED
    struct timeval updated_time;
    RECEOVED_STATE received_state;
    size_t received_byte;
    uint32_t payload_len;
    uint64_t now_playing_revision;
    char h_p_buf[SP_HEADER_SIZE + SP_MAX_PAYLOAD];
} client_t;

typedef struct client_arr_elem
{
    client_t *client;
    struct client_arr_elem *next;
} client_arr_elem_t;

/* Returns a listening socket, or -1 on error. Caller closes the socket. */
int tcp_open_listener(int port);

/* Start the TCP control thread and return immediately.
 * Returns 0 on success or an error number on failure.
 * On success, the caller signals stop_evt_fd and joins *out_thread before
 * closing either fd. out_thread is only valid after a successful call. */
int tcp_accept_loop(int serv_sock, int stop_evt_fd, pthread_t *out_thread);

/* Called by the future audio supplier. Metadata is copied; the caller retains
 * ownership of track->title_utf8. Pass NULL with stream_state=0 to stop. */
int tcp_server_update_broadcast(const SpNowPlaying *track,
                                uint64_t live_pts_ms, uint8_t stream_state);

#endif
