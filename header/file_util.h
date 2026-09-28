#pragma once

#define MAX_FILE_NAME 255
#define MAX_FILE_NUMBER 5
//files : malloced. need to be collected after usage.
int8_t get_files(char files[],const size_t MAX_FILE_STR_SIZE,const char *dirname);