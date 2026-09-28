#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_FILE_NAME 255
#define MAX_FILE_NUMBER 5
//files : malloced. need to be collected after usage.
int8_t get_files(char files[],const size_t MAX_FILE_STR_SIZE,const char *dirname);

int8_t send_file_str_len(const int file_str_len,int cli_sock);

int8_t send_file_list(const char files[],const int file_str_len,int cli_sock);

int8_t send_initial_files_info(const char files[],const int file_str_len,int cli_sock);

ssize_t write_all(int fd, const void *buf, size_t total);
