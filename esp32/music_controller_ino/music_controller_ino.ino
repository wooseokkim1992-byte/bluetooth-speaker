#include <Arduino.h>
#include <Network.h>
#include <cstring>
#include "speaker_lcd.h"
#include "speaker_tasks.h"
#include "timer_types.h"  // Needed by Arduino's prototypes for legacy timer.ino.

namespace {
constexpr uint8_t kPlaybackButtonPin = 32;
constexpr uint8_t kConnectionButtonPin = 33;
constexpr uint8_t kClientIdButtonPin = 27;
constexpr uint8_t kVolumePin = 34;
constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kVolumePollMs = 50;
constexpr uint32_t kLcdPollMs = 20;
constexpr uint32_t kVolumeDisplayMs = 1000;
constexpr uint32_t kClientIdScrollMs = 1000;
constexpr size_t kLcdColumns = 16;
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
Button client_id_button{kClientIdButtonPin};
uint32_t last_volume_poll_ms = 0;
uint32_t last_volume_log_ms = 0;
uint32_t last_volume_change_ms = 0;
uint32_t last_lcd_poll_ms = 0;
bool volume_display_pending = false;
int volume_neutral_adc = 2048;
int filtered_volume_adc = 2048;
int current_volume_percent = 50;
bool client_id_visible = false;
bool client_id_valid = false;
char client_id_digits[21] = {};
size_t client_id_length = 0;
size_t client_id_offset = 0;
int8_t client_id_scroll_direction = 1;
uint32_t last_client_id_scroll_ms = 0;

bool load_client_id_for_display() {
  uint64_t client_id = 0;
  if (!get_saved_speaker_client_id(&client_id)) return false;
  snprintf(client_id_digits, sizeof(client_id_digits), "%llu",
           static_cast<unsigned long long>(client_id));
  client_id_length = strlen(client_id_digits);
  client_id_offset = 0;
  client_id_scroll_direction = 1;
  Serial.print("client id: ");
  Serial.println(client_id_digits);
  return true;
}

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
    last_volume_change_ms = now_ms;
    volume_display_pending = true;
    if (now_ms - last_volume_log_ms >= 250) {
      Serial.printf("VRy ADC=%d filtered=%d volume=%d%%\n",
                    adc, filtered_volume_adc, current_volume_percent);
      last_volume_log_ms = now_ms;
    }
  }
}

void make_lcd_title(const char *utf8, char *display) {
  size_t column = 0;
  for (size_t i = 0; utf8[i] != '\0' && column < 16; ++i) {
    const uint8_t byte = static_cast<uint8_t>(utf8[i]);
    if (byte < 0x80) {
      display[column++] = byte >= 0x20 && byte <= 0x7e ? byte : ' ';
    } else if ((byte & 0xc0) != 0x80) {
      // Standard HD44780 character ROM cannot decode UTF-8/Korean glyphs.
      display[column++] = '?';
    }
  }
  display[column] = '\0';
}

void poll_lcd(uint32_t now_ms) {
  if (now_ms - last_lcd_poll_ms < kLcdPollMs) return;
  last_lcd_poll_ms = now_ms;

  if (client_id_visible) {
    if (now_ms - last_client_id_scroll_ms >= kClientIdScrollMs) {
      last_client_id_scroll_ms = now_ms;
      if (!client_id_valid) {
        client_id_valid = load_client_id_for_display();
      } else if (client_id_length > kLcdColumns) {
        const size_t last_offset = client_id_length - kLcdColumns;
        if (client_id_scroll_direction > 0) {
          ++client_id_offset;
          if (client_id_offset == last_offset) client_id_scroll_direction = -1;
        } else {
          --client_id_offset;
          if (client_id_offset == 0) client_id_scroll_direction = 1;
        }
      }
    }
    if (!client_id_valid) {
      speaker_lcd_show("Client ID", "ID not ready");
    } else {
      char visible_digits[kLcdColumns + 1] = {};
      const size_t remaining = client_id_length - client_id_offset;
      const size_t count = remaining < kLcdColumns ? remaining : kLcdColumns;
      memcpy(visible_digits, client_id_digits + client_id_offset, count);
      speaker_lcd_show("Client ID", visible_digits);
    }
    return;
  }

  const SpeakerDisplayState state = get_speaker_display_state();
  switch (state) {
    case SpeakerDisplayState::WifiConnecting:
      speaker_lcd_show("wifi-connecting", "");
      return;
    case SpeakerDisplayState::WifiError:
      speaker_lcd_show("wifi-err", "");
      return;
    case SpeakerDisplayState::ConnectServer:
      speaker_lcd_show("connect server", "");
      return;
    case SpeakerDisplayState::Disconnecting:
      speaker_lcd_show("disconnecting..", "");
      return;
    case SpeakerDisplayState::Disconnected:
      speaker_lcd_show("disconnected", "");
      return;
    case SpeakerDisplayState::Playing:
    case SpeakerDisplayState::Paused:
      break;
  }

  uint64_t track_id = 0;
  char title_utf8[65] = {};
  const bool has_title =
      get_speaker_now_playing(&track_id, title_utf8, sizeof(title_utf8));

  char second_line[17] = {};
  if (volume_display_pending &&
      now_ms - last_volume_change_ms < kVolumeDisplayMs) {
    snprintf(second_line, sizeof(second_line), "Volume: %d%%",
             current_volume_percent);
  } else {
    volume_display_pending = false;
    if (has_title) make_lcd_title(title_utf8, second_line);
  }
  speaker_lcd_show(state == SpeakerDisplayState::Paused ? "Paused" : "Playing",
                   second_line);
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
  init_button(client_id_button);
  init_volume_input();
  speaker_lcd_begin();
  if (!start_speaker_tasks()) {
    Serial.println("Failed to start speaker tasks");
  }
}

void loop() {
  const uint32_t now_ms = millis();
  poll_volume(now_ms);
  poll_lcd(now_ms);
  if (pressed(playback_button, now_ms) &&
      !post_speaker_command(SpeakerCommand::TogglePlayback)) {
    Serial.println("Failed to queue playback button press");
  }
  if (pressed(connection_button, now_ms)) {
    Serial.println("GPIO33 connection button pressed");
    const SpeakerDisplayState before = get_speaker_display_state();
    if (!post_speaker_command(SpeakerCommand::ToggleConnection)) {
      Serial.println("Failed to queue connection button press");
    } else if (!client_id_visible &&
               (before == SpeakerDisplayState::Playing ||
                before == SpeakerDisplayState::Paused)) {
      // Show the transition even if DISCONNECT_ACK arrives before the next poll.
      speaker_lcd_show("disconnecting..", "");
    }
  }
  if (pressed(client_id_button, now_ms)) {
    client_id_visible = !client_id_visible;
    if (client_id_visible) {
      client_id_valid = load_client_id_for_display();
      last_client_id_scroll_ms = now_ms;
    }
    last_lcd_poll_ms = now_ms - kLcdPollMs;
    poll_lcd(now_ms);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
