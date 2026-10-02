#ifndef SIGNAL_UTIL_H
#define SIGNAL_UTIL_H

/* Register SIGINT handling. Returns 0 on success, -1 on error (errno set). */
int setup_sigint_handler(void);

/* Nonzero after SIGINT has requested shutdown. */
int is_stop_requested(void);

#endif
