#include <Arduino.h>
#include <Network.h>
#include "speaker_tasks.h"
#include "timer_types.h"  // Needed by Arduino's prototypes for legacy timer.ino.

namespace {
constexpr uint8_t kPlaybackButtonPin = 32;
constexpr uint8_t kConnectionButtonPin = 33;
constexpr uint8_t kVolumePin = 34;
constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kVolumePollMs = 50;
constexpr int kVolumeDeadzoneAdc = 450;
constexpr int kVolumeStepPercent = 2;

struct Button {
  uint8_t pin;
  uint8_t last_raw = LOW;
  uint8_t stable = LOW;
  uint32_t changed_at_ms = 0;
};

Button playback_button{kPlaybackButtonPin};
Button connection_button{kConnectionButtonPin};
uint32_t last_volume_poll_ms = 0;
uint32_t last_volume_log_ms = 0;
int volume_neutral_adc = 2048;
int filtered_volume_adc = 2048;
int current_volume_percent = 50;

void init_volume_input() {
  analogReadResolution(12);
  analogSetPinAttenuation(kVolumePin, ADC_11db);
  int sum = 0;
  for (int i = 0; i < 8; ++i) {
    sum += analogRead(kVolumePin);
    delay(2);
  }
  volume_neutral_adc = sum / 8;
  filtered_volume_adc = volume_neutral_adc;
  last_volume_poll_ms = millis();
  Serial.printf("VRy neutral ADC=%d, initial volume=%d%%\n",
                volume_neutral_adc, current_volume_percent);
}

void poll_volume(uint32_t now_ms) {
  if (now_ms - last_volume_poll_ms < kVolumePollMs) return;
  last_volume_poll_ms = now_ms;
  const int adc = analogRead(kVolumePin);
  filtered_volume_adc = (filtered_volume_adc * 3 + adc) / 4;
  int next_percent = current_volume_percent;
  if (filtered_volume_adc > volume_neutral_adc + kVolumeDeadzoneAdc) {
    next_percent = min(100, next_percent + kVolumeStepPercent);
  } else if (filtered_volume_adc < volume_neutral_adc - kVolumeDeadzoneAdc) {
    next_percent = max(0, next_percent - kVolumeStepPercent);
  }
  if (next_percent != current_volume_percent &&
      set_speaker_volume_percent(next_percent)) {
    current_volume_percent = next_percent;
    if (now_ms - last_volume_log_ms >= 250) {
      Serial.printf("VRy ADC=%d filtered=%d volume=%d%%\n",
                    adc, filtered_volume_adc, current_volume_percent);
      last_volume_log_ms = now_ms;
    }
  }
}

void init_button(Button &button) {
  pinMode(button.pin, INPUT_PULLDOWN);
  button.last_raw = digitalRead(button.pin);
  button.stable = button.last_raw;
  button.changed_at_ms = millis();
}

bool pressed(Button &button, uint32_t now_ms) {
  const uint8_t raw = digitalRead(button.pin);
  if (raw != button.last_raw) {
    button.last_raw = raw;
    button.changed_at_ms = now_ms;
  }
  if (raw == button.stable || now_ms - button.changed_at_ms < kDebounceMs) {
    return false;
  }
  button.stable = raw;
  return button.stable == HIGH;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  init_button(playback_button);
  init_button(connection_button);
  init_volume_input();
  if (!start_speaker_tasks()) {
    Serial.println("Failed to start speaker tasks");
  }
}

void loop() {
  const uint32_t now_ms = millis();
  poll_volume(now_ms);
  if (pressed(playback_button, now_ms) &&
      !post_speaker_command(SpeakerCommand::TogglePlayback)) {
    Serial.println("Failed to queue playback button press");
  }
  if (pressed(connection_button, now_ms)) {
    Serial.println("GPIO33 connection button pressed");
    if (!post_speaker_command(SpeakerCommand::ToggleConnection)) {
      Serial.println("Failed to queue connection button press");
    }
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
