#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// #include <bluetooth/bluetooth.h>
// #include <bluetooth/rfcomm.h>
#include <sys/stat.h>         // 파일 정보 확인
#include <signal.h>           // 종료 신호 처리
#include <poll.h>             // 소켓 이벤트 대기
#include <netdb.h>            // 호스트 이름 조회
// #include <bluetooth/sdp.h>    // SPP 서비스 검색·등록
// #include <bluetooth/sdp_lib.h>

#define MAX_LISTEN 5

typedef struct _TCP_control_thread_params_t{
    struct sockaddr_in cli_info;
    int cli_sock;
}TCP_control_thread_params_t;

static volatile sig_atomic_t stop_flag = 0;

static void handle_sigint(int signo){
    if(signo==SIGINT){
        stop_flag=1;
    }
}

void* TCP_control_thread(void*data){
    TCP_control_thread_params_t *param_data = (TCP_control_thread_params_t *)data;
    char *cli_ip = inet_ntoa(param_data->cli_info.sin_addr);
    fprintf(stdout,"%s connected\n",cli_ip);
    fprintf(stdout,"client socket number: %d\n",param_data->cli_sock);
    close(param_data->cli_sock);
    free(data);
    param_data=NULL;
    return NULL;
}

int main(int argc, const char*argv[]){
    if(argc!=2){
        fprintf(stderr,"usage %s <port number>\n",argv[0]);
        return EXIT_FAILURE;
    }
    const int PORT = atoi(argv[1]);
    int serv_sock;
    struct sockaddr_in s_addr;
    struct sockaddr_in cli_addr;
    if((serv_sock = socket(PF_INET,SOCK_STREAM,0))<0){
        perror("failed to create socket");
        return EXIT_FAILURE;
    }
    memset(&s_addr,0,sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr.sin_port = htons(PORT);
    
    if(bind(serv_sock,(struct sockaddr*)&s_addr,sizeof(s_addr))==-1){
        fprintf(stderr,"failed to set up server socket in port: %d\n",PORT);
        return EXIT_FAILURE;
    }
    
    if(listen(serv_sock,MAX_LISTEN)==-1){
        perror("failed during listen process");
        return EXIT_FAILURE;
    }
    //setting sigaction
    struct sigaction action; 
    memset(&action,0,sizeof(action));
    action.sa_handler =  handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if(sigaction(SIGINT,&action,NULL)==-1){
        perror("error during register sigaction");
        return EXIT_FAILURE;
    }
    
    while(1){
        int cli_sock;
        socklen_t clnt_addr_size=sizeof(cli_addr);
        if((cli_sock=accept(serv_sock,(struct sockaddr*)&cli_addr,&clnt_addr_size))==-1){
            perror("failed to create connection");
            if(errno!=EINTR)continue;
            else {
                if(stop_flag){
                    close(cli_sock);
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
    close(serv_sock);

    return EXIT_SUCCESS;
}