#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "file_util.h"
#include "signal_util.h"
#include "tcp_interface.h"

#define MAX_LISTEN 5

typedef struct _TCP_control_thread_params_t{
    struct sockaddr_in cli_info;
    int cli_sock;
}TCP_control_thread_params_t;



static void* TCP_control_thread(void*data){
    TCP_control_thread_params_t *param_data = (TCP_control_thread_params_t *)data;
    char *cli_ip = inet_ntoa(param_data->cli_info.sin_addr);
    int cli_sock = param_data->cli_sock;
    fprintf(stdout,"%s connected\n",cli_ip);
    fprintf(stdout,"client socket number: %d\n",param_data->cli_sock);
    const char *file_dir = "./files";
    char files[MAX_FILE_NAME*MAX_FILE_NUMBER];
    if(get_files(files,(const size_t)sizeof(files),file_dir)){
        perror("failed to get file list\n");
        close(param_data->cli_sock);
        free(data);
        param_data=NULL;
        return NULL;
    }
    int file_str_len = strlen(files);
    printf("files : %s\n",files);
    printf("files str len %d\n",file_str_len);
    if(send_initial_files_info(files,file_str_len,param_data->cli_sock)){
        perror("failed to send file list\n");
        close(param_data->cli_sock);
        free(data);
        param_data=NULL;
        return NULL;
    }
    //epoll 설정
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_struct ev = {0,};
    ev.event = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    ev.data.fd = cli_sock;
    epoll_ctl(epfd,EPOL_CTL_ADD,cli_sock,&ev);

    close(param_data->cli_sock);
    free(data);
    param_data=NULL;
    return NULL;
}

ssize_t write_all(int fd, const void *buf, size_t total){
    if(total > SSIZE_MAX){
        errno = EOVERFLOW;
        return -1;
    }
    const unsigned char *bytes = buf;
    size_t offset = 0;

    while(offset < total){
        ssize_t written = write(fd, bytes + offset, total - offset);
        if(written > 0){
            offset += (size_t)written;
        }else if(written < 0 && errno == EINTR){
            continue;
        }else{
            if(written == 0) errno = EIO;
            return -1;
        }
    }
    return (ssize_t)offset;
}


int8_t send_file_str_len(const int file_str_len,int cli_sock){
    if(file_str_len < 0) return 1;
    uint32_t network_byte = htonl((uint32_t)file_str_len);
    if(write_all(cli_sock, &network_byte, sizeof(network_byte)) != (ssize_t)sizeof(network_byte)){
        return 1;
    }
    return 0;
}

int8_t send_file_list(const char files[],const int file_str_len,int cli_sock){
    if(file_str_len < 0) return 1;
    if(write_all(cli_sock, files, (size_t)file_str_len) != (ssize_t)file_str_len){
        return 1;
    }
    return 0;
}

int8_t send_initial_files_info(const char files[],const int file_str_len,int cli_sock){
    if(send_file_str_len(file_str_len,cli_sock)){
        return 1;
    }
    if(send_file_list(files,file_str_len,cli_sock)){
        return 1;
    }
    return 0;
}

int tcp_open_listener(int port)
{
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock < 0) {
        perror("failed to create socket");
        return -1;
    }

    struct sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr.sin_port = htons(port);

    if (bind(serv_sock, (struct sockaddr *)&s_addr, sizeof(s_addr)) == -1) {
        fprintf(stderr, "failed to set up server socket in port: %d\n", port);
        close(serv_sock);
        return -1;
    }
    if (listen(serv_sock, MAX_LISTEN) == -1) {
        perror("failed during listen process");
        close(serv_sock);
        return -1;
    }
    return serv_sock;
}

void tcp_accept_loop(int serv_sock)
{
    struct sockaddr_in cli_addr;
    while(1){
        int cli_sock;
        socklen_t clnt_addr_size=sizeof(cli_addr);
        if((cli_sock=accept(serv_sock,(struct sockaddr*)&cli_addr,&clnt_addr_size))==-1){
            perror("failed to create connection");
            if(errno!=EINTR)continue;
            else {
                if(is_stop_requested()){
                    perror("SIGINT\n");
                    break;
                }
                continue;
            }
        }
        
        pthread_t pid;
        TCP_control_thread_params_t *input_data=malloc(sizeof(TCP_control_thread_params_t));
        if(!input_data){
            perror("failed to allocate data");
            close(cli_sock);
            continue;
        }
        input_data->cli_sock = cli_sock;
        input_data->cli_info = cli_addr;
        if(pthread_create(&pid,NULL,TCP_control_thread,(void*)input_data)!=0){
            perror("failed to create TCP Control Thread\n");
            close(cli_sock);
            free(input_data);
        }else{
            pthread_detach(pid);
        }
    }
}
