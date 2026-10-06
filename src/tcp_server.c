#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include "file_util.h"
#include "signal_util.h"
#define MAX_LISTEN 10
#define MAX_EPOLL 10

typedef struct _TCP_control_thread_params_t
{
    int server_fd;
    int event_fd;
} TCP_control_thread_params_t;

client_arr_elem_t clients[CLIENT_BUCKET_COUNT] = {0};
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static int8_t register_fd_to_clients(int fd, client_arr_elem_t *clients);
static client_t *get_client_data(int fd, client_arr_elem_t *clients);
static int8_t erase_client_data(int fd, client_arr_elem_t *clients);
static int8_t init_cli_buf_state(client_t *cli);
static int8_t scrap_data(int fd, char buf[], size_t buf_size, client_t *cli);
static void print_client_data(const client_t *cli);
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
        fprintf(stderr, "failed to create epool");
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
            free(param);
            data = NULL;
            return NULL;
        }

        struct epoll_event evt_list[CLIENT_BUCKET_COUNT + 2];

        while (1)
        {
            int evt_cnt = epoll_wait(epoll_fd, evt_list, CLIENT_BUCKET_COUNT + 2, -1);
            if (evt_cnt < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            int8_t break_flag = 0;
            for (int8_t i = 0; i < evt_cnt; i++)
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
                            init_cli_buf_state(cli);
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
    }
    close(epoll_fd);
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
    int err = pthread_create(out_thread, NULL, TCP_control_thread, params);
    if (err != 0)
    {
        free(params);
    }
    return err;
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

static void print_client_data(const client_t *cli)
{
    if (cli == NULL)
        return;

    size_t data_len = cli->received_byte;
    if (data_len > sizeof(cli->h_p_buf))
        data_len = sizeof(cli->h_p_buf);

    printf("client_t: fd=%d client_id=%u status=%d "
           "updated_time={sec=%lld,usec=%ld} received_state=%d "
           "received_byte=%zu payload_len=%u\n",
           cli->fd, (unsigned)cli->client_id, (int)cli->status,
           (long long)cli->updated_time.tv_sec,
           (long)cli->updated_time.tv_usec,
           (int)cli->received_state, cli->received_byte,
           (unsigned)cli->payload_len);

    printf("h_p_buf (%zu bytes):", data_len);
    for (size_t i = 0; i < data_len; ++i)
    {
        if (i % 16 == 0)
            printf("\n  %04zx: ", i);
        printf("%02x ", (unsigned)(unsigned char)cli->h_p_buf[i]);
    }
    putchar('\n');
    fflush(stdout);
}
