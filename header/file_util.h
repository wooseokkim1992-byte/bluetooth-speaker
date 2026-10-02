#ifndef FILE_UTIL_H
#define FILE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define MAX_FILE_NAME 255
#define MAX_FILE_NUMBER 5

/* Caller owns files; max_file_str_size includes the terminating NUL. */
int8_t get_files(char files[], size_t max_file_str_size, const char *dirname);

#endif
