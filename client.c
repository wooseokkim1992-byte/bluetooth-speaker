#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_FILE_LIST_BYTES (64U * 1024U)

static ssize_t read_all(int fd, void *buf, size_t total) {
    unsigned char *bytes = buf;
    size_t offset = 0;

    while (offset < total) {
        ssize_t n = read(fd, bytes + offset, total - offset);
        if (n > 0) {
            offset += (size_t)n;
        } else if (n == 0) {
            return (ssize_t)offset;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return (ssize_t)offset;
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

    uint32_t network_len;
    ssize_t received = read_all(sock, &network_len, sizeof(network_len));
    if (received < 0) {
        perror("read file list length");
        close(sock);
        return EXIT_FAILURE;
    }
    if (received != (ssize_t)sizeof(network_len)) {
        fprintf(stderr, "server closed before sending file list length\n");
        close(sock);
        return EXIT_FAILURE;
    }

    uint32_t file_list_len = ntohl(network_len);
    if (file_list_len > MAX_FILE_LIST_BYTES) {
        fprintf(stderr, "file list is too large: %u bytes\n", (unsigned)file_list_len);
        close(sock);
        return EXIT_FAILURE;
    }

    char *files = malloc((size_t)file_list_len + 1);
    if (files == NULL) {
        perror("malloc");
        close(sock);
        return EXIT_FAILURE;
    }

    received = read_all(sock, files, file_list_len);
    if (received < 0) {
        perror("read file list");
        free(files);
        close(sock);
        return EXIT_FAILURE;
    }
    if (received != (ssize_t)file_list_len) {
        fprintf(stderr, "server closed before sending the complete file list\n");
        free(files);
        close(sock);
        return EXIT_FAILURE;
    }
    files[file_list_len] = '\0';

    printf("file list (%u bytes):\n", (unsigned)file_list_len);
    puts(files);
    free(files);
    close(sock);
    return EXIT_SUCCESS;
}
