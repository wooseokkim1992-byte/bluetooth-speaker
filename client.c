#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(sock);
        return EXIT_FAILURE;
    }

    printf("connected to %s:%ld\n", argv[1], port);
    close(sock);
    return EXIT_SUCCESS;
}
