#include "timer_types.h"
#include <Network.h>

extern volatile bool is_wifi_connected;

timer_obj_t timer_obj = {};

void setup() {
  if(set_wifi_connection()){
    is_wifi_connected=true;
    NetworkClient client;
    if(set_File_server_connection(&client)){
      get_files_info(client);
      if (!init_timer(&timer_obj)) {
        Serial.println("Timer initialization failed");
      }
    }
  }
}

void loop() {
  // put your main code here, to run repeatedly:
  if (timer_obj.tickSignal != nullptr &&
      xSemaphoreTake(timer_obj.tickSignal, 0) == pdTRUE) {
    Serial.println("1-second timer tick");
  }
}
