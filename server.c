#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
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

    int stop_evt_fd = eventfd(0,EFD_CLOEXEC | EFD_NONBLOCK);
    if(stop_evt_fd<0){
        perror("error during registering event fd");
        close(serv_sock);
        return EXIT_FAILURE;
    }
    // The worker inherits this mask; SIGINT is handled only by main.
    sigset_t stop_signals, previous_mask;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    int err = pthread_sigmask(SIG_BLOCK, &stop_signals, &previous_mask);
    if(err != 0){
        fprintf(stderr, "failed to block SIGINT: %s\n", strerror(err));
        close(stop_evt_fd);
        close(serv_sock);
        return EXIT_FAILURE;
    }

    pthread_t tcp_thread;
    err = tcp_accept_loop(serv_sock, stop_evt_fd, &tcp_thread);
    if(err != 0){
        fprintf(stderr, "failed to create TCP control thread: %s\n", strerror(err));
        pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
        close(stop_evt_fd);
        close(serv_sock);
        return EXIT_FAILURE;
    }

    sigset_t wait_mask = previous_mask;
    sigdelset(&wait_mask, SIGINT);
    while(!is_stop_requested()){
        // Atomically unblock SIGINT and sleep, avoiding a flag/pause race.
        sigsuspend(&wait_mask);
    }

    uint64_t stop_evt_sig = 1;
    ssize_t written;
    do{
        written = write(stop_evt_fd, &stop_evt_sig, sizeof(stop_evt_sig));
    }while(written < 0 && errno == EINTR);
    if(written != (ssize_t)sizeof(stop_evt_sig)){
        perror("failed to notify TCP control thread");
        return EXIT_FAILURE;
    }

    // Wait only after notifying the worker, before closing shared fds.
    err = pthread_join(tcp_thread, NULL);
    if(err != 0){
        fprintf(stderr, "failed to join TCP control thread: %s\n", strerror(err));
        return EXIT_FAILURE;
    }
    close(stop_evt_fd);
    close(serv_sock);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    return EXIT_SUCCESS;
}
