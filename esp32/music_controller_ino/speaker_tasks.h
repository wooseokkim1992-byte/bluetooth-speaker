#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>

// The controller's BLE callback must enqueue a command, not access the TCP
// socket directly. Only the network task owns the socket.
enum class SpeakerCommand : uint8_t {
  Connect,
  Disconnect,
  Pause,
  Resume,
};

bool start_speaker_tasks();
bool post_speaker_command(SpeakerCommand command);

// Copies the latest UTF-8 title into title (NUL-terminated). Returns false
// until a NOW_PLAYING frame has been received on the current connection.
bool get_speaker_now_playing(uint64_t *track_id, char *title,
                             size_t title_capacity);

struct DeviceIdentity {
  uint64_t client_id;
  uint64_t token;      // 최초 등록 전에는 0
};

bool load_or_create_identity(DeviceIdentity &identity);
uint32_t next_request_id(uint32_t &sequence);
