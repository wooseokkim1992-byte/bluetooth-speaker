#include <Arduino.h>
#include <Network.h>
#include "speaker_tasks.h"
#include "timer_types.h"  // Needed by Arduino's prototypes for legacy timer.ino.

void setup() {
  Serial.begin(115200);
  if (!start_speaker_tasks()) {
    Serial.println("Failed to start speaker tasks");
  } else if (!post_speaker_command(SpeakerCommand::Connect)) {
    // Temporary bring-up path. Move this command to the BLE controller later.
    Serial.println("Failed to queue initial connect request");
  }
}

void loop() {
  // Arduino already runs loop() in a FreeRTOS task. Keep socket, decoder,
  // and BLE work out of this task; BLE callbacks will enqueue commands.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
