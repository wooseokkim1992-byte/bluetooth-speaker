#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "signal_util.h"
#include "tcp_server.h"

int main(int argc, const char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage %s <port number>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const int port = atoi(argv[1]);
    int serv_sock = tcp_open_listener(port);
    if (serv_sock == -1)
        return EXIT_FAILURE;

    if (setup_sigint_handler() == -1) {
        perror("error during register sigaction");
        close(serv_sock);
        return EXIT_FAILURE;
    }

    tcp_accept_loop(serv_sock);
    close(serv_sock);
    return EXIT_SUCCESS;
}
