#include "signal_util.h"

#include <signal.h>
#include <string.h>

volatile sig_atomic_t stop_flag = 0;

static void handle_sigint(int signo){
    if(signo==SIGINT){
        stop_flag=1;
    }
}

int setup_sigint_handler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGINT, &action, NULL);
}

int is_stop_requested(void)
{
    return stop_flag != 0;
}
