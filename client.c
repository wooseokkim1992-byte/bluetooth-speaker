#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "tcp_interface.h"

#define CLIENT_POLL_TIMEOUT_MS 1000
#define HEADER_PAYLOAD_GAP_MS 100

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int signo) {
    (void)signo;
    stop_requested = 1;
}

static uint32_t next_request_id(uint32_t *next_id) {
    uint32_t id = *next_id;
    *next_id = id + 1;
    if (*next_id == SP_NO_REQUEST) *next_id = 1;
    return id;
}

static int send_request(int sock, uint8_t type, uint32_t request_id,
                        const uint8_t *payload, size_t payload_len) {
    if (payload_len > SP_MAX_PAYLOAD || (payload_len > 0 && payload == NULL)) {
        errno = EINVAL;
        return -1;
    }

    SpHeader header = {
        .magic = SP_MAGIC,
        .version = SP_VERSION,
        .type = type,
        .flags = 0,
        .request_id = request_id,
        .payload_len = (uint32_t)payload_len,
    };
    char header_bytes[SP_HEADER_SIZE];
    if (construct_header(&header, header_bytes, sizeof(header_bytes)) != 0) {
        return -1;
    }
    if (write_all(sock, header_bytes, sizeof(header_bytes)) !=
        (ssize_t)sizeof(header_bytes)) {
        return -1;
    }

    if (payload_len > 0) {
        struct timespec gap = {
            .tv_sec = 0,
            .tv_nsec = HEADER_PAYLOAD_GAP_MS * 1000000L,
        };
        while (nanosleep(&gap, &gap) == -1 && errno == EINTR) {
            /* Complete the header/payload gap even if a signal arrives. */
        }
        if (write_all(sock, payload, payload_len) != (ssize_t)payload_len) {
            return -1;
        }
    }

    printf("sent type=0x%02x request_id=%u header=%u payload=%zu bytes\n",
           type, request_id, SP_HEADER_SIZE, payload_len);
    fflush(stdout);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <server IPv4 address> <port number> [client_id u64]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port number: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    uint64_t client_id = 1;
    if (argc == 4) {
        errno = 0;
        unsigned long long parsed_id = strtoull(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' || argv[3][0] == '-') {
            fprintf(stderr, "invalid client_id: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        client_id = (uint64_t)parsed_id;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, argv[1], &server_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    struct sigaction ignore_sigpipe = {.sa_handler = SIG_IGN};
    if (sigaction(SIGPIPE, &ignore_sigpipe, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        if (stop_requested) {
            puts("connection cancelled");
            close(sock);
            return EXIT_SUCCESS;
        }
        perror("connect");
        close(sock);
        return EXIT_FAILURE;
    }

    uint32_t request_id = 1;
    SpConnectRequest connect_req = {.client_id_utf8 = client_id, .token = 0};
    SpHeader connect_header = {
        .type = SP_CONNECT_REQ,
        .payload_len = SP_CONNECT_REQ_PAYLOAD_SIZE,
    };
    uint8_t connect_payload[SP_CONNECT_REQ_PAYLOAD_SIZE];
    if (construct_connect_request_payload(&connect_header, &connect_req,
                                          (char *)connect_payload,
                                          sizeof(connect_payload)) != 0 ||
        send_request(sock, SP_CONNECT_REQ, next_request_id(&request_id),
                     connect_payload, sizeof(connect_payload)) != 0) {
        perror("CONNECT_REQ");
        close(sock);
        return EXIT_FAILURE;
    }

    printf("connected to %s:%ld\n", argv[1], port);
    puts("PING every second; press Ctrl+C to send DISCONNECT_REQ");
    fflush(stdout);

    struct pollfd server = {.fd = sock, .events = POLLIN};
    unsigned char buffer[4096];
    int exit_status = EXIT_SUCCESS;

    while (!stop_requested) {
        // A finite timeout also covers a signal just before entering poll().
        int ready = poll(&server, 1, CLIENT_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (stop_requested) break;
        if (ready == 0) {
            if (send_request(sock, SP_PING, next_request_id(&request_id),
                             NULL, 0) != 0) {
                perror("PING");
                exit_status = EXIT_FAILURE;
                break;
            }
            continue;
        }

        if (server.revents & POLLNVAL) {
            fprintf(stderr, "invalid server socket\n");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (server.revents & POLLERR) {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                           &socket_error, &error_len) == -1) {
                perror("getsockopt");
                exit_status = EXIT_FAILURE;
                break;
            }
            if (socket_error != 0) {
                fprintf(stderr, "server connection error: %s\n",
                        strerror(socket_error));
                exit_status = EXIT_FAILURE;
                break;
            }
        }

        if (server.revents & (POLLIN | POLLHUP)) {
            ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
            if (received > 0) {
                // The current server has no application protocol responses yet.
                printf("received %zd bytes\n", received);
                fflush(stdout);
            } else if (received == 0) {
                puts("server closed the connection");
                break;
            } else if (errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                perror("recv");
                exit_status = EXIT_FAILURE;
                break;
            }
        }
    }

    if (stop_requested) {
        if (send_request(sock, SP_DISCONNECT_REQ,
                         next_request_id(&request_id), NULL, 0) != 0) {
            perror("DISCONNECT_REQ");
            exit_status = EXIT_FAILURE;
        }
    }
    close(sock);
    return exit_status;
}
