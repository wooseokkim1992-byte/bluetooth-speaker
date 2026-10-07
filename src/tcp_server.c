#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include "file_util.h"
#include "signal_util.h"
#define MAX_LISTEN 10
#define MAX_EPOLL 10
#define PCM_SAMPLE_RATE 44100u
#define PCM_BYTES_PER_SAMPLE_FRAME 4u
#define AUDIO_CHUNK_SAMPLES 882u /* 20 ms at 44.1 kHz */
#define AUDIO_CHUNK_BYTES (AUDIO_CHUNK_SAMPLES * PCM_BYTES_PER_SAMPLE_FRAME)
#define AUDIO_QUEUE_CAPACITY 8u
#define MAX_DECODED_PCM_BYTES (64u * 1024u * 1024u)

typedef struct _TCP_control_thread_params_t
{
    int server_fd;
    int event_fd;
    uint8_t *pcm_data;
    size_t pcm_len;
} TCP_control_thread_params_t;

typedef struct
{
    uint64_t pts_ms;
    uint8_t bytes[AUDIO_CHUNK_BYTES];
} audio_chunk_t;

typedef struct
{
    int wake_fd;
    pthread_mutex_t mutex;
    atomic_bool stop;
    const uint8_t *pcm_data;
    size_t pcm_len;
    size_t pcm_offset;
    uint64_t emitted_samples;
    audio_chunk_t queue[AUDIO_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_count;
} audio_stream_t;

client_arr_elem_t clients[CLIENT_BUCKET_COUNT] = {0};
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    uint8_t stream_state;
    uint64_t live_pts_ms;
    uint64_t track_id;
    uint16_t title_len;
    uint8_t title[SP_MAX_PAYLOAD - SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE];
    uint64_t revision;
} broadcast_state_t;

static broadcast_state_t broadcast_state = {0};
static pthread_mutex_t broadcast_mutex = PTHREAD_MUTEX_INITIALIZER;

static int8_t register_fd_to_clients(int fd, client_arr_elem_t *clients);
static client_t *get_client_data(int fd, client_arr_elem_t *clients);
static int8_t erase_client_data(int fd, client_arr_elem_t *clients);
static int8_t init_cli_buf_state(client_t *cli);
static int8_t scrap_data(int fd, char buf[], size_t buf_size, client_t *cli);
static int8_t set_and_response(client_t *cli);
static int8_t send_current_now_playing(client_t *cli);
static int8_t get_broadcast_position(uint64_t *live_pts_ms,
                                     uint8_t *stream_state);
static int load_demo_pcm(uint8_t **out_data, size_t *out_len);
static int start_audio_stream(audio_stream_t *stream, pthread_t *out_thread,
                              int epoll_fd, const uint8_t *pcm_data,
                              size_t pcm_len);
static void stop_audio_stream(audio_stream_t *stream, pthread_t thread);
static void *audio_producer_thread(void *arg);
static void dispatch_audio_chunks(audio_stream_t *stream, int epoll_fd);
static int8_t send_response_frame(const client_t *cli, uint8_t type,
                                  uint32_t request_id, void *payload,
                                  uint32_t payload_len);
static void print_client_data(const client_t *cli);
static void print_sp_header(const SpHeader *header);
static void print_sp_payload(const SpHeader *header, const char *frame,
                             size_t frame_size);
static void *TCP_control_thread(void *param)
{
    TCP_control_thread_params_t *data = (TCP_control_thread_params_t *)param;
    int server_fd = data->server_fd;
    int event_fd = data->event_fd;
    struct sockaddr_in cli_addr_in = {
        0,
    };
    socklen_t cli_addr_len = sizeof(cli_addr_in);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        perror("failed to create epoll");
        free(data->pcm_data);
        free(param);
        return NULL;
    }
    else
    {
        struct epoll_event serv_ev;
        serv_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
        serv_ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &serv_ev) < 0)
        {
            perror("Failed to register epoll.\n");
            close(epoll_fd);
            free(data->pcm_data);
            free(param);
            data = NULL;
            return NULL;
        }
        struct epoll_event event_ev;
        event_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
        event_ev.data.fd = event_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &event_ev) < 0)
        {
            perror("Failed to register epoll.\n");
            close(epoll_fd);
            free(data->pcm_data);
            free(param);
            data = NULL;
            return NULL;
        }

        audio_stream_t audio_stream;
        pthread_t audio_thread;
        if (start_audio_stream(&audio_stream, &audio_thread, epoll_fd,
                               data->pcm_data, data->pcm_len) != 0)
        {
            perror("failed to start audio stream");
            close(epoll_fd);
            free(data->pcm_data);
            free(param);
            return NULL;
        }

        struct epoll_event evt_list[CLIENT_BUCKET_COUNT + 3];

        while (1)
        {
            int evt_cnt = epoll_wait(epoll_fd, evt_list, CLIENT_BUCKET_COUNT + 3, -1);
            if (evt_cnt < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            int8_t break_flag = 0;
            for (int i = 0; i < evt_cnt; i++)
            {
                if (evt_list[i].data.fd == server_fd)
                {
                    if (evt_list[i].events & EPOLLIN)
                    {
                        memset(&cli_addr_in, 0, sizeof(cli_addr_in));
                        int cli_sock = accept(server_fd, (struct sockaddr *)&cli_addr_in, &cli_addr_len);
                        if (cli_sock >= 0)
                        {
                            char ip[16];
                            if (inet_ntop(AF_INET, &cli_addr_in.sin_addr, ip, sizeof(ip)) != NULL)
                            {
                                fprintf(stdout, "ip:%s connected!\n", ip);
                            }
                            struct epoll_event cli_evt;
                            cli_evt.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP;
                            cli_evt.data.fd = cli_sock;
                            // 접속 해제 할 때 (EPOLLHUP | EPOLLERR | EPOLLRDHUP)이벤드 발생시 free 해줄예정
                            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cli_sock, &cli_evt) >= 0)
                            {
                                register_fd_to_clients(cli_sock, clients);
                                puts("client registered\n");
                            }
                        }
                    }
                    else
                    {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, &evt_list[i]);
                        perror("main socket problem\n");
                        break_flag = 1;
                    }
                }
                else if (evt_list[i].data.fd == event_fd)
                {
                    if (evt_list[i].events & EPOLLIN)
                    {
                        puts("Interrupt Signal Detected\n");
                        break_flag = 1;
                        break;
                    }
                }
                else if (evt_list[i].data.fd == audio_stream.wake_fd)
                {
                    if (evt_list[i].events & EPOLLIN)
                    {
                        uint64_t wake_count;
                        ssize_t n;
                        do
                        {
                            n = read(audio_stream.wake_fd, &wake_count,
                                     sizeof(wake_count));
                        } while (n < 0 && errno == EINTR);
                        if (n == (ssize_t)sizeof(wake_count))
                            dispatch_audio_chunks(&audio_stream, epoll_fd);
                    }
                }
                else
                {
                    int temp_fd = evt_list[i].data.fd;
                    int close_client = 0;
                    if (evt_list[i].events & EPOLLIN)
                    {
                        client_t *cli = get_client_data(temp_fd, clients);
                        if (cli == NULL || scrap_data(temp_fd, cli->h_p_buf,
                                                      sizeof(cli->h_p_buf), cli) != 0)
                        {
                            close_client = 1;
                        }
                        else if (cli->received_state == PAYLOAD)
                        {
                            print_client_data(cli);
                            int8_t result = set_and_response(cli);
                            if (result != 0)
                                close_client = 1;
                            else if (init_cli_buf_state(cli) != 0)
                                close_client = 1;
                        }
                    }
                    else if (evt_list[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
                    {
                        close_client = 1;
                    }

                    if (close_client)
                    {
                        puts("connection closed");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, temp_fd, NULL);
                        erase_client_data(temp_fd, clients);
                        close(temp_fd);
                    }
                }
            }
            if (break_flag)
                break;
        }
        stop_audio_stream(&audio_stream, audio_thread);
    }
    close(epoll_fd);
    free(data->pcm_data);
    free(param);
    data = NULL;
    return NULL;
}

int tcp_open_listener(int port)
{
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock < 0)
    {
        perror("failed to create socket");
        return -1;
    }

    struct sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr.sin_port = htons(port);

    if (bind(serv_sock, (struct sockaddr *)&s_addr, sizeof(s_addr)) == -1)
    {
        fprintf(stderr, "failed to set up server socket in port: %d\n", port);
        close(serv_sock);
        return -1;
    }
    if (listen(serv_sock, CLIENT_BUCKET_COUNT) == -1)
    {
        perror("failed during listen process");
        close(serv_sock);
        return -1;
    }
    return serv_sock;
}

int tcp_accept_loop(int serv_sock, int event_fd, pthread_t *out_thread)
{
    if (out_thread == NULL)
    {
        return EINVAL;
    }
    TCP_control_thread_params_t *params;
    params = malloc(sizeof(TCP_control_thread_params_t));
    // 생성한 thread 에 넘기고, 해당 thread 가 종료 되어질 때,free 될 예정.
    if (params == NULL)
    {
        return ENOMEM;
    }
    params->event_fd = event_fd;
    params->server_fd = serv_sock;
    if (load_demo_pcm(&params->pcm_data, &params->pcm_len) != 0)
    {
        int load_error = errno != 0 ? errno : EIO;
        free(params);
        return load_error;
    }
    int err = pthread_create(out_thread, NULL, TCP_control_thread, params);
    if (err != 0)
    {
        free(params->pcm_data);
        free(params);
    }
    return err;
}

static int load_demo_pcm(uint8_t **out_data, size_t *out_len)
{
    size_t capacity = 1024u * 1024u;
    size_t length = 0;
    uint8_t *pcm = malloc(capacity);
    if (pcm == NULL)
        return -1;

    /* The input path is fixed; no user-provided text is passed to the shell. */
    FILE *decoder = popen("ffmpeg -nostdin -hide_banner -loglevel error "
                          "-i files/1.mp3 -f s16le -acodec pcm_s16le "
                          "-ar 44100 -ac 2 pipe:1", "r");
    if (decoder == NULL)
    {
        free(pcm);
        return -1;
    }

    int failure_errno = 0;
    for (;;)
    {
        if (length == capacity)
        {
            if (capacity == MAX_DECODED_PCM_BYTES)
            {
                failure_errno = EFBIG;
                break;
            }
            size_t new_capacity = capacity * 2;
            if (new_capacity > MAX_DECODED_PCM_BYTES)
                new_capacity = MAX_DECODED_PCM_BYTES;
            uint8_t *larger = realloc(pcm, new_capacity);
            if (larger == NULL)
            {
                failure_errno = ENOMEM;
                break;
            }
            pcm = larger;
            capacity = new_capacity;
        }
        size_t n = fread(pcm + length, 1, capacity - length, decoder);
        length += n;
        if (n == 0)
        {
            if (ferror(decoder))
                failure_errno = EIO;
            break;
        }
    }
    if (failure_errno != 0)
    {
        uint8_t discard[8192];
        while (fread(discard, 1, sizeof(discard), decoder) != 0)
        {
            /* Drain the decoder pipe before pclose() to avoid blocking it. */
        }
    }
    int decoder_status = pclose(decoder);
    if (failure_errno != 0 || decoder_status != 0 || length == 0 ||
        length % PCM_BYTES_PER_SAMPLE_FRAME != 0)
    {
        errno = failure_errno != 0 ? failure_errno : EIO;
        free(pcm);
        return -1;
    }

    *out_data = pcm;
    *out_len = length;
    fprintf(stdout, "decoded files/1.mp3: %zu PCM bytes, 44.1 kHz stereo\n",
            length);
    return 0;
}

static int start_audio_stream(audio_stream_t *stream, pthread_t *out_thread,
                              int epoll_fd, const uint8_t *pcm_data,
                              size_t pcm_len)
{
    memset(stream, 0, sizeof(*stream));
    stream->wake_fd = -1;
    stream->pcm_data = pcm_data;
    stream->pcm_len = pcm_len;
    atomic_init(&stream->stop, 0);
    int err = pthread_mutex_init(&stream->mutex, NULL);
    if (err != 0)
    {
        errno = err;
        return -1;
    }

    stream->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stream->wake_fd < 0)
    {
        pthread_mutex_destroy(&stream->mutex);
        return -1;
    }
    struct epoll_event audio_event = {
        .events = EPOLLIN | EPOLLERR,
        .data.fd = stream->wake_fd,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, stream->wake_fd,
                  &audio_event) != 0)
    {
        close(stream->wake_fd);
        pthread_mutex_destroy(&stream->mutex);
        return -1;
    }
    err = pthread_create(out_thread, NULL, audio_producer_thread, stream);
    if (err != 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, stream->wake_fd, NULL);
        close(stream->wake_fd);
        pthread_mutex_destroy(&stream->mutex);
        errno = err;
        return -1;
    }
    return 0;
}

static void stop_audio_stream(audio_stream_t *stream, pthread_t thread)
{
    atomic_store(&stream->stop, 1);
    pthread_join(thread, NULL);
    close(stream->wake_fd);
    pthread_mutex_destroy(&stream->mutex);
    tcp_server_update_broadcast(NULL, 0, 0);
}

static void *audio_producer_thread(void *arg)
{
    audio_stream_t *stream = arg;
    static const uint8_t title[] = "1.mp3";
    SpNowPlaying track = {
        .track_id = 1,
        .title_len = sizeof(title) - 1,
        .title_utf8 = title,
    };
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return NULL;

    while (!atomic_load(&stream->stop))
    {
        audio_chunk_t chunk = {
            .pts_ms = stream->emitted_samples * 1000u / PCM_SAMPLE_RATE,
        };
        size_t copied = 0;
        while (copied < AUDIO_CHUNK_BYTES)
        {
            size_t remaining = stream->pcm_len - stream->pcm_offset;
            size_t count = AUDIO_CHUNK_BYTES - copied;
            if (count > remaining)
                count = remaining;
            memcpy(chunk.bytes + copied, stream->pcm_data + stream->pcm_offset,
                   count);
            copied += count;
            stream->pcm_offset = (stream->pcm_offset + count) % stream->pcm_len;
        }
        stream->emitted_samples += AUDIO_CHUNK_SAMPLES;

        if (tcp_server_update_broadcast(&track, chunk.pts_ms, 1) != 0)
            break;
        pthread_mutex_lock(&stream->mutex);
        if (stream->queue_count == AUDIO_QUEUE_CAPACITY)
        {
            stream->queue_head = (stream->queue_head + 1) % AUDIO_QUEUE_CAPACITY;
            --stream->queue_count;
        }
        size_t tail = (stream->queue_head + stream->queue_count) %
                      AUDIO_QUEUE_CAPACITY;
        stream->queue[tail] = chunk;
        ++stream->queue_count;
        pthread_mutex_unlock(&stream->mutex);

        uint64_t one = 1;
        ssize_t wake;
        do
        {
            wake = write(stream->wake_fd, &one, sizeof(one));
        } while (wake < 0 && errno == EINTR);
        if (wake < 0 && errno != EAGAIN)
            break;

        deadline.tv_nsec += 20000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_nsec -= 1000000000L;
            ++deadline.tv_sec;
        }
        int sleep_error;
        do
        {
            sleep_error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                          &deadline, NULL);
        } while (sleep_error == EINTR && !atomic_load(&stream->stop));
        if (sleep_error != 0 && sleep_error != EINTR)
            break;
    }
    tcp_server_update_broadcast(NULL, 0, 0);
    return NULL;
}

static void dispatch_audio_chunks(audio_stream_t *stream, int epoll_fd)
{
    for (;;)
    {
        audio_chunk_t chunk;
        pthread_mutex_lock(&stream->mutex);
        if (stream->queue_count == 0)
        {
            pthread_mutex_unlock(&stream->mutex);
            break;
        }
        chunk = stream->queue[stream->queue_head];
        stream->queue_head = (stream->queue_head + 1) % AUDIO_QUEUE_CAPACITY;
        --stream->queue_count;
        pthread_mutex_unlock(&stream->mutex);

        SpAudioData audio = {
            .stream_pts_ms = chunk.pts_ms,
            .data = chunk.bytes,
            .data_len = AUDIO_CHUNK_BYTES,
        };
        for (size_t bucket = 0; bucket < CLIENT_BUCKET_COUNT; ++bucket)
        {
            client_arr_elem_t *node = &clients[bucket];
            while (node != NULL && node->client != NULL)
            {
                client_t *cli = node->client;
                int fd = cli->fd;
                if (cli->status == PLAYING &&
                    send_response_frame(cli, SP_AUDIO_DATA, SP_NO_REQUEST,
                                        &audio,
                                        SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE +
                                            audio.data_len) != 0)
                {
                    perror("audio send failed; disconnecting client");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    erase_client_data(fd, clients);
                    close(fd);
                    node = &clients[bucket];
                    continue;
                }
                node = node->next;
            }
        }
    }
}

int tcp_server_update_broadcast(const SpNowPlaying *track,
                                uint64_t live_pts_ms, uint8_t stream_state)
{
    if (stream_state > 1 || (stream_state == 1 &&
                             (track == NULL ||
                              track->title_len > sizeof(broadcast_state.title) ||
                              (track->title_len != 0 && track->title_utf8 == NULL))))
    {
        errno = EINVAL;
        return -1;
    }

    int lock_error = pthread_mutex_lock(&broadcast_mutex);
    if (lock_error != 0)
    {
        errno = lock_error;
        return -1;
    }

    int changed = broadcast_state.stream_state != stream_state;
    if (stream_state == 1)
    {
        changed = changed || broadcast_state.track_id != track->track_id ||
                  broadcast_state.title_len != track->title_len ||
                  (track->title_len != 0 &&
                   memcmp(broadcast_state.title, track->title_utf8,
                          track->title_len) != 0);
        if (changed)
        {
            broadcast_state.track_id = track->track_id;
            broadcast_state.title_len = track->title_len;
            if (track->title_len != 0)
                memcpy(broadcast_state.title, track->title_utf8, track->title_len);
        }
    }
    else
    {
        broadcast_state.track_id = 0;
        broadcast_state.title_len = 0;
    }
    broadcast_state.live_pts_ms = stream_state == 1 ? live_pts_ms : 0;
    broadcast_state.stream_state = stream_state;
    if (changed)
    {
        ++broadcast_state.revision;
        if (broadcast_state.revision == 0)
            ++broadcast_state.revision;
    }
    pthread_mutex_unlock(&broadcast_mutex);
    return 0;
}

static int8_t register_fd_to_clients(int fd, client_arr_elem_t clients[])
{
    if (fd < 0 || clients == NULL)
    {
        return -1;
    }
    int32_t hashed_index = fd % CLIENT_BUCKET_COUNT;
    client_arr_elem_t *temp = &clients[hashed_index];
    while (temp->client != NULL)
    {
        if (temp->client->fd == fd)
        {
            return -1;
        }
        if (temp->next == NULL)
        {
            break;
        }
        temp = temp->next;
    }

    struct timeval updated_time;
    if (gettimeofday(&updated_time, NULL) != 0)
    {
        perror("failed to init timeval\n");
        return -1;
    }
    client_t *new_client_info = malloc(sizeof(*new_client_info));
    if (new_client_info == NULL)
    {
        return -1;
    }
    *new_client_info = (client_t){
        .fd = fd,
        .client_id = 0,
        .status = INIT,
        .updated_time = updated_time,
        .received_state = NOTHING,
        .received_byte = 0,
        .payload_len = 0,
        .now_playing_revision = 0,
    };
    if (temp->client == NULL)
    {
        temp->client = new_client_info;
    }
    else
    {
        client_arr_elem_t *new_elem = malloc(sizeof(*new_elem));
        if (new_elem == NULL)
        {
            free(new_client_info);
            return -1;
        }
        new_elem->client = new_client_info;
        new_elem->next = NULL;
        temp->next = new_elem;
    }
    return 0;
}

static client_t *get_client_data(int fd, client_arr_elem_t *clients)
{
    if (fd < 0 || clients == NULL)
    {
        return NULL;
    }
    int8_t hashed_index = fd % CLIENT_BUCKET_COUNT;
    client_arr_elem_t *temp = &clients[hashed_index];

    while (temp != NULL && temp->client != NULL)
    {
        if (temp->client->fd == fd)
        {
            return temp->client;
        }
        temp = temp->next;
    }
    return NULL;
}

static int8_t erase_client_data(int fd, client_arr_elem_t *clients)
{
    if (fd < 0 || clients == NULL)
    {
        return -1;
    }
    int8_t hashed_index = fd % CLIENT_BUCKET_COUNT;
    client_arr_elem_t *temp = &clients[hashed_index];
    if (temp->client && temp->client->fd == fd)
    {
        free(temp->client);
        if (temp->next != NULL)
        {
            client_arr_elem_t *next = temp->next;
            *temp = *next;
            free(next);
        }
        else
        {
            temp->client = NULL;
        }
    }
    else
    {
        while (temp->next && temp->next->client && temp->next->client->fd != fd)
        {
            temp = temp->next;
        }
        if (temp->next == NULL || temp->next->client == NULL)
        {
            return -1;
        }
        client_arr_elem_t *obj_to_rm = temp->next;
        temp->next = obj_to_rm->next;
        free(obj_to_rm->client);
        free(obj_to_rm);
    }

    return 0;
}

static int8_t scrap_data(int fd, char buf[], size_t buf_size, client_t *cli)
{
    // return value
    // 0: success
    //-1: size error,cli ==NULL
    //-2: connection error
    //-3 : wrong magic num
    if (cli == NULL)
    {
        perror("client object not initailized!\n");
        return -1;
    }
    if (buf_size > SSIZE_MAX || cli->received_byte > buf_size)
    {
        perror("not proper buf size!\n");
        return -1;
    }

    size_t target_size;
    if (cli->received_state == NOTHING)
        target_size = SP_HEADER_SIZE;
    else if (cli->received_state == HEADER)
        target_size = SP_HEADER_SIZE + (size_t)cli->payload_len;
    else
        return -1;

    if (target_size > buf_size || cli->received_byte >= target_size)
        return -1;

    size_t should_read = target_size - cli->received_byte;
    ssize_t read_bytes;
    do
    {
        read_bytes = read(fd, buf + cli->received_byte, should_read);
    } while (read_bytes < 0 && errno == EINTR);

    if (read_bytes == 0)
    {
        return -2;
    }
    if (read_bytes < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        perror("read failed");
        return -2;
    }

    cli->received_byte += (size_t)read_bytes;
    if (cli->received_state == HEADER && cli->received_byte == SP_HEADER_SIZE + (size_t)cli->payload_len)
    {
        cli->received_state = PAYLOAD;
    }
    else if (cli->received_state == NOTHING && cli->received_byte >= SP_HEADER_SIZE)
    {
        uint32_t network_payload_len;
        memcpy(&network_payload_len, buf + 12, sizeof(network_payload_len));
        uint32_t payload_len = ntohl(network_payload_len);
        if (payload_len > SP_MAX_PAYLOAD)
        {
            return -2;
        }
        cli->payload_len = payload_len;
        cli->received_state = payload_len == 0 ? PAYLOAD : HEADER;
    }
    return 0;
}

static int8_t init_cli_buf_state(client_t *cli)
{
    if (cli == NULL)
        return -1;
    cli->payload_len = 0;
    cli->received_byte = 0;
    cli->received_state = NOTHING;
    memset(&(cli->h_p_buf), 0, sizeof(cli->h_p_buf));
    return 0;
}

/* Only the TCP control thread writes to client sockets. A partial nonblocking
 * send that cannot finish is treated as a broken connection. */
static int8_t send_response_frame(const client_t *cli, uint8_t type,
                                  uint32_t request_id, void *payload,
                                  uint32_t payload_len)
{
    if (cli == NULL || payload_len > SP_MAX_PAYLOAD ||
        (payload_len != 0 && payload == NULL))
    {
        errno = EINVAL;
        return -1;
    }

    SpHeader header = {
        .magic = SP_MAGIC,
        .version = SP_VERSION,
        .type = type,
        .flags = 0,
        .request_id = request_id,
        .payload_len = payload_len,
    };
    char frame[SP_HEADER_SIZE + SP_MAX_PAYLOAD];
    if (construct_header(&header, frame, SP_HEADER_SIZE) != 0)
        return -1;

    int8_t encoded = 0;
    switch (type)
    {
    case SP_CONNECT_ACK:
        encoded = construct_connect_payload(&header, payload,
                                            frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_PONG:
        encoded = construct_pong_payload(&header, payload,
                                         frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_PAUSE_ACK:
        encoded = construct_pause_payload(&header, payload,
                                          frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_RESUME_ACK:
        encoded = construct_resume_payload(&header, payload,
                                           frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_NOW_PLAYING:
        encoded = construct_Now_Playing_payload(&header, payload,
                                                frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_AUDIO_DATA:
        encoded = construct_audio_payload(&header, payload,
                                          frame + SP_HEADER_SIZE, payload_len);
        break;
    case SP_DISCONNECT_ACK:
        if (payload_len != 0)
            encoded = -1;
        break;
    default:
        encoded = -1;
        break;
    }
    if (encoded != 0)
    {
        errno = EINVAL;
        return -1;
    }

    size_t frame_len = SP_HEADER_SIZE + (size_t)payload_len;
    size_t sent = 0;
    while (sent < frame_len)
    {
        ssize_t n = send(cli->fd, frame + sent, frame_len - sent,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
        {
            if (n == 0)
                errno = EPIPE;
            return -1;
        }
    }
    return 0;
}

static int8_t get_broadcast_position(uint64_t *live_pts_ms,
                                     uint8_t *stream_state)
{
    int lock_error = pthread_mutex_lock(&broadcast_mutex);
    if (lock_error != 0)
    {
        errno = lock_error;
        return -1;
    }
    *live_pts_ms = broadcast_state.live_pts_ms;
    *stream_state = broadcast_state.stream_state;
    pthread_mutex_unlock(&broadcast_mutex);
    return 0;
}

/* A changed song is announced by the TCP thread on connect or the next PING.
 * No audio thread writes directly to client sockets. */
static int8_t send_current_now_playing(client_t *cli)
{
    uint8_t title[SP_MAX_PAYLOAD - SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE];
    uint64_t track_id;
    uint64_t revision;
    uint16_t title_len;

    int lock_error = pthread_mutex_lock(&broadcast_mutex);
    if (lock_error != 0)
    {
        errno = lock_error;
        return -1;
    }
    if (broadcast_state.stream_state == 0 ||
        broadcast_state.revision == cli->now_playing_revision)
    {
        pthread_mutex_unlock(&broadcast_mutex);
        return 0;
    }
    track_id = broadcast_state.track_id;
    title_len = broadcast_state.title_len;
    revision = broadcast_state.revision;
    if (title_len != 0)
        memcpy(title, broadcast_state.title, title_len);
    pthread_mutex_unlock(&broadcast_mutex);

    SpNowPlaying now_playing = {
        .track_id = track_id,
        .title_len = title_len,
        .title_utf8 = title_len != 0 ? title : NULL,
    };
    if (send_response_frame(cli, SP_NOW_PLAYING, SP_NO_REQUEST,
                            &now_playing,
                            SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE + title_len) != 0)
        return -1;
    cli->now_playing_revision = revision;
    return 0;
}

/* 0: keep connection, 1: close after ACK, -1: protocol/I/O error. */
static int8_t set_and_response(client_t *cli)
{
    if (cli == NULL || cli->received_byte < SP_HEADER_SIZE)
        return -1;

    SpHeader request;
    if (parsing_header(&request, cli->h_p_buf, SP_HEADER_SIZE) != 0)
        return -1;
    print_sp_header(&request);
    if (check_magic_num(&request) != 0 || request.version != SP_VERSION ||
        request.flags != 0 || request.request_id == SP_NO_REQUEST ||
        request.payload_len != cli->payload_len ||
        cli->received_byte != SP_HEADER_SIZE + (size_t)request.payload_len)
    {
        fprintf(stderr, "invalid request header on fd %d\n", cli->fd);
        return -1;
    }
    print_sp_payload(&request, cli->h_p_buf, cli->received_byte);

    switch (request.type)
    {
    case SP_CONNECT_REQ:
    {
        if (cli->status != INIT)
            return -1;
        SpConnectRequest connect_req;
        if (parsing_payload_connect_REQ(&request, cli->h_p_buf,
                                        cli->received_byte, &connect_req) != 0)
            return -1;

        uint64_t live_pts_ms;
        uint8_t stream_state;
        if (get_broadcast_position(&live_pts_ms, &stream_state) != 0)
            return -1;
        /* Temporary Base profile until the database is connected. */
        uint64_t token = connect_req.token;
        if (!token)
        {
            if (generate_token(&token) != 0)
            {
                perror("generate_token");
                return -1;
            }
        }
        SpConnectAck ack = {
            .result = 0,
            .plan = 1,
            .codec = SP_CODEC_PCM_S16LE,
            .bitrate_bps = PCM_SAMPLE_RATE * 16u * 2u,
            .sample_rate_hz = 44100,
            .channels = 2,
            .live_pts_ms = stream_state == 1 ? live_pts_ms : 0,
            .ping_interval_ms = 1000,
            .pong_timeout_ms = 10000,
            .token = token,
        };
        if (send_response_frame(cli, SP_CONNECT_ACK, request.request_id,
                                &ack, SP_CONNECT_ACK_PAYLOAD_SIZE) != 0)
            return -1;
        cli->client_id = connect_req.client_id_utf8;
        cli->status = PLAYING;
        if (send_current_now_playing(cli) != 0)
            return -1;
        break;
    }
    case SP_PING:
    {
        if (request.payload_len != 0 || cli->status == INIT ||
            cli->status == STOPPING)
            return -1;
        SpPong pong;
        if (get_broadcast_position(&pong.live_pts_ms,
                                   &pong.stream_state) != 0)
            return -1;
        if (send_response_frame(cli, SP_PONG, request.request_id,
                                &pong, SP_PONG_PAYLOAD_SIZE) != 0)
            return -1;
        if (send_current_now_playing(cli) != 0)
            return -1;
        break;
    }
    case SP_PAUSE_REQ:
    {
        if (request.payload_len != 0 || cli->status != PLAYING)
            return -1;
        SpPauseAck ack = {.result = 0};
        if (send_response_frame(cli, SP_PAUSE_ACK, request.request_id,
                                &ack, SP_PAUSE_ACK_PAYLOAD_SIZE) != 0)
            return -1;
        cli->status = PAUSED;
        break;
    }
    case SP_RESUME_REQ:
    {
        if (request.payload_len != 0 || cli->status != PAUSED)
            return -1;
        uint64_t live_pts_ms;
        uint8_t stream_state;
        if (get_broadcast_position(&live_pts_ms, &stream_state) != 0)
            return -1;
        SpResumeAck ack = {
            .result = 0,
            .live_pts_ms = stream_state == 1 ? live_pts_ms : 0,
        };
        if (send_response_frame(cli, SP_RESUME_ACK, request.request_id,
                                &ack, SP_RESUME_ACK_PAYLOAD_SIZE) != 0)
            return -1;
        cli->status = PLAYING;
        if (send_current_now_playing(cli) != 0)
            return -1;
        break;
    }
    case SP_DISCONNECT_REQ:
        if (request.payload_len != 0 || cli->status == INIT)
            return -1;
        if (send_response_frame(cli, SP_DISCONNECT_ACK, request.request_id,
                                NULL, 0) != 0)
            return -1;
        cli->status = STOPPING;
        return 1;
    default:
        fprintf(stderr, "unsupported request type 0x%02x on fd %d\n",
                request.type, cli->fd);
        return -1;
    }

    if (gettimeofday(&cli->updated_time, NULL) != 0)
        return -1;
    return 0;
}

static void
print_client_data(const client_t *cli)
{
    if (cli == NULL)
        return;

    printf("client_t: fd=%d client_id=%" PRIu64 " status=%d "
           "updated_time={sec=%lld,usec=%ld} received_state=%d "
           "received_byte=%zu payload_len=%u\n",
           cli->fd, cli->client_id, (int)cli->status,
           (long long)cli->updated_time.tv_sec,
           (long)cli->updated_time.tv_usec,
           (int)cli->received_state, cli->received_byte,
           (unsigned)cli->payload_len);

    fflush(stdout);
}

static void print_sp_header(const SpHeader *header)
{
    if (header == NULL)
        return;

    printf("SpHeader: magic=0x%08x version=%u type=0x%02x "
           "flags=0x%04x request_id=%u payload_len=%u\n",
           (unsigned)header->magic, (unsigned)header->version,
           (unsigned)header->type, (unsigned)header->flags,
           (unsigned)header->request_id, (unsigned)header->payload_len);
    fflush(stdout);
}

static void print_sp_payload(const SpHeader *header, const char *frame,
                             size_t frame_size)
{
    if (header == NULL || frame == NULL || frame_size < SP_HEADER_SIZE ||
        header->payload_len > frame_size - SP_HEADER_SIZE)
        return;

    const uint8_t *payload = (const uint8_t *)frame + SP_HEADER_SIZE;
    if (header->payload_len == 0)
    {
        puts("Payload: none");
    }
    else if (header->type == SP_CONNECT_REQ &&
             header->payload_len == SP_CONNECT_REQ_PAYLOAD_SIZE)
    {
        printf("Payload CONNECT_REQ: client_id=%" PRIu64 " token=%s\n",
               ntoh64(payload), ntoh64(payload + 8) == 0 ? "none" : "<redacted>");
    }
    else
    {
        printf("Payload: type=0x%02x length=%u (not decoded)\n",
               (unsigned)header->type, (unsigned)header->payload_len);
    }
    fflush(stdout);
}
