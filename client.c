#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#define CLIENT_POLL_TIMEOUT_MS 1000

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int signo) {
    (void)signo;
    stop_requested = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <server IPv4 address> <port number>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port number: %s\n", argv[2]);
        return EXIT_FAILURE;
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

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        close(sock);
        return EXIT_FAILURE;
    }

    printf("connected to %s:%ld\n", argv[1], port);
    puts("waiting for server data or shutdown; press Ctrl+C to disconnect");
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
        if (ready == 0) continue;

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

    if (stop_requested) puts("disconnect requested");
    close(sock);
    return exit_status;
}
