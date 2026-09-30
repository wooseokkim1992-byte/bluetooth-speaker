#include <WiFi.h>

const char* ssid = "SSID";
const char* password = "PASSWORD";
volatile bool is_wifi_connected=false;

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

void setup() {
  if(set_wifi_connection()){
    is_wifi_connected=true;
    NetworkClient client;
    if(set_File_server_connection(&client)){
      get_files_info(client);
    }
  }
}

void loop() {
  // put your main code here, to run repeatedly:

}
