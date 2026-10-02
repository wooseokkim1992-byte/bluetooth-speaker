#include "file_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int8_t get_files(char files[],const size_t MAX_FILE_STR_SIZE,const char *dirname){
    DIR *dir = opendir(dirname);
    if(dir==NULL){
        perror("failed to open download directory");
        return 1;
    }
    size_t files_len = 0;
    struct dirent *entry;
    files[0]='\0';
    while((entry=readdir(dir))!=NULL){
        struct stat st;
        if(fstatat(dirfd(dir),entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<=-1){
            fprintf(stderr,"failed to load file info\n");
            continue;
        }
        if(S_ISREG(st.st_mode)){
            fprintf(stdout,"file name : %s\n",entry->d_name);

            if(files_len+strlen(entry->d_name)+2<MAX_FILE_STR_SIZE){
                files_len += strlen(entry->d_name);
                strcat(files,entry->d_name);
                files[files_len++]=',';
                files[files_len]='\0';
            }else{
                break;
            }
        }
    }
    if(files_len > 0) files[files_len-1]='\0';
    closedir(dir);
    return 0;
}
