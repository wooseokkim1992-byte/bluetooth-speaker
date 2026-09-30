#include <Network.h>
#include <lwip/def.h>

const char* SERVER_IP = "SERVER IP";
const uint16_t SERVER_PORT = 9000;

bool set_File_server_connection(NetworkClient *cli){
  if(cli==NULL){
    return false;
  }
  cli->setTimeout(5000);
  if(!cli->connect(SERVER_IP,SERVER_PORT)){
    Serial.println("TCP connection failed!");
    return false;
  }
  Serial.println("TCP connection finished");
  return true;
}

ssize_t read_all(NetworkClient cli,char *buf,size_t buf_size){
  if(buf_size>SSIZE_MAX){
    return -1;
  }
  size_t amount_read=0;
  while(amount_read<buf_size){
    size_t n = cli.readBytes(
      buf+amount_read,buf_size-amount_read
    );
    if(n==0)return -1;
    amount_read += n;
  }
  return static_cast<ssize_t>(amount_read);
}

bool get_str_len(NetworkClient cli,uint32_t *length){
  if(length==nullptr)return false;
  uint32_t len_net_bytes;
  if(read_all(cli,reinterpret_cast<char*>(&len_net_bytes),sizeof(len_net_bytes))
      !=static_cast<ssize_t>(sizeof(len_net_bytes))){
    return false;
  }
  *length = ntohl(len_net_bytes);
  
  return true;
}

bool get_files_info(NetworkClient cli){
  uint32_t files_str_length;
  if(!get_str_len(cli,&files_str_length)){
    Serial.println("Failed to read file list length (4 bytes).");
    return false;
  }
  return true;
}
