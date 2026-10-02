#include <Network.h>
#include <lwip/def.h>
#include <WiFi.h>

const char* ssid = "ASUS_90_2G";
const char* password = "seafood_3633";
volatile bool is_wifi_connected=false;
const char *SERVER_IP = "192.168.1.160";
const uint16_t SERVER_PORT = 9000;

#define MAX_FILE_NUM 10

bool set_wifi_connection(){
  Serial.begin(115200);
  int count = WiFi.scanNetworks();
  Serial.print("Networks found : ");
  Serial.println(count);
  for (int i = 0; i < count; i++) {
    Serial.println(WiFi.SSID(i));
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);

  unsigned long start = millis();
  while(WiFi.status()!=WL_CONNECTED &&
    millis() - start <20000
  ){
    delay(500);
    Serial.print(".");
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.println("\n WiFi connected!"); 
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  }else{
    Serial.println("\nWi-Fi connection failed.");
    return false;
  }
  return true;
}

bool set_File_server_connection(NetworkClient *cli) {
  if (cli == NULL) {
    return false;
  }
  cli->setTimeout(5000);
  if (!cli->connect(SERVER_IP, SERVER_PORT)) {
    Serial.println("TCP connection failed!");
    WiFi.disconnect();
    return false;
  }
  Serial.println("TCP connection finished");
  return true;
}

ssize_t read_all(NetworkClient cli, char *buf, size_t buf_size) {
  if (buf_size > SSIZE_MAX) {
    return -1;
  }
  size_t amount_read = 0;
  while (amount_read < buf_size) {
    size_t n = cli.readBytes(
      buf + amount_read, buf_size - amount_read);
    if (n == 0) return -1;
    amount_read += n;
  }
  return static_cast<ssize_t>(amount_read);
}

bool get_str_len(NetworkClient cli, uint32_t *length) {
  if (length == nullptr) return false;
  uint32_t len_net_bytes;
  if (read_all(cli, reinterpret_cast<char *>(&len_net_bytes), sizeof(len_net_bytes))
      != static_cast<ssize_t>(sizeof(len_net_bytes))) {
    return false;
  }
  *length = ntohl(len_net_bytes);

  return true;
}

bool get_files_str(NetworkClient cli, char *file_list[], const uint32_t lenth) {
}

bool get_files_info(NetworkClient cli) {
  uint32_t files_str_length;
  if (!get_str_len(cli, &files_str_length)) {
    Serial.println("Failed to read file list length (4 bytes).");
    return false;
  }
  return true;
}
