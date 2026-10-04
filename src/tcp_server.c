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
#include <sys/epoll.h>
#include <errno.h>

#include "file_util.h"
#include "signal_util.h"
#include "tcp_interface.h"
#define MAX_LISTEN 10
#define MAX_EPOLL 10

typedef struct _TCP_control_thread_params_t{
    int server_fd;
    int event_fd;
}TCP_control_thread_params_t;

static void* TCP_control_thread(void*param){
    TCP_control_thread_params_t*data = (TCP_control_thread_params_t*)param;
    int server_fd = data->server_fd;
    int event_fd= data->event_fd;
    struct sockaddr_in cli_addr_in={0,};
    socklen_t cli_addr_len = sizeof(cli_addr_in);

    int epoll_fd = epoll_create1(0);
    if(epoll_fd<0){
        fprintf(stderr,"failed to create epool");
    }else{
        struct epoll_event serv_ev;
        serv_ev.events=EPOLLIN | EPOLLRDHUP| EPOLLERR;
        serv_ev.data.fd = server_fd;
        if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,server_fd,&serv_ev)<0){
            perror("Failed to register epoll.\n");
            close(epoll_fd);
            free(param);
            data=NULL;
            return NULL;
        }
        struct epoll_event event_ev;
        event_ev.events=  EPOLLIN | EPOLLRDHUP| EPOLLERR;
        event_ev.data.fd = event_fd;
        if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,event_fd,&event_ev)<0){
            perror("Failed to register epoll.\n");
            close(epoll_fd);
            free(param);
            data=NULL;
            return NULL;
        }
        
        struct epoll_event evt_list[MAX_EPOLL+2];

        while(1){
            int evt_cnt = epoll_wait(epoll_fd,evt_list,MAX_EPOLL+2,-1);
            if(evt_cnt<0){
                if(errno==EINTR)continue;
                break;
            }
            int8_t break_flag=0;
            for(int8_t i =0;i<evt_cnt;i++){
                if(evt_list[i].data.fd==server_fd){
                    if(evt_list[i].events&EPOLLIN){
                        memset(&cli_addr_in,0,sizeof(cli_addr_in));
                        int cli_sock = accept(server_fd,(struct sockaddr*)&cli_addr_in,&cli_addr_len);
                        if(cli_sock>=0){
                            char ip[16];
                            if(inet_ntop(AF_INET,&cli_addr_in.sin_addr,ip,sizeof(ip))!=NULL){
                                fprintf(stdout,"ip:%s connected!\n",ip);
                            }
                            struct epoll_event cli_evt;
                            cli_evt.events = EPOLLIN|EPOLLHUP|EPOLLERR|EPOLLRDHUP;
                            cli_evt.data.fd = cli_sock;
                            if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,cli_sock,&cli_evt)>=0){
                                puts("client registered\n");
                            }
                        }
                    }else{
                        epoll_ctl(epoll_fd,EPOLL_CTL_DEL,server_fd,&evt_list[i]);
                        perror("main socket problem\n");
                        break_flag=1;
                    }
                }else if(evt_list[i].data.fd==event_fd){
                    if(evt_list[i].events&EPOLLIN){
                        puts("Interrupt Signal Detected\n");
                        break_flag=1;
                        break;
                    }
                }else{
                    if(evt_list[i].events&(EPOLLHUP|EPOLLERR|EPOLLRDHUP)){
                        puts("connection closed");
                        epoll_ctl(epoll_fd,EPOLL_CTL_DEL,evt_list[i].data.fd,&evt_list[i]);
                    }else if(evt_list[i].events&EPOLLIN){}
                    
                }
            }
            if(break_flag)break;
        }
    }
    close(epoll_fd);
    free(param);
    data=NULL;
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

int tcp_accept_loop(int serv_sock, int event_fd, pthread_t *out_thread)
{
    if(out_thread == NULL){
        return EINVAL;
    }
    TCP_control_thread_params_t* params;
    params = malloc(sizeof(TCP_control_thread_params_t));
    // 생성한 thread 에 넘기고, 해당 thread 가 종료 되어질 때,free 될 예정.
    if(params==NULL){
        return ENOMEM;
    }
    params->event_fd=event_fd;
    params->server_fd=serv_sock;
    int err = pthread_create(out_thread, NULL, TCP_control_thread, params);
    if(err != 0){
        free(params);
    }
    return err;
}
