#include "speaker_tasks.h"
#include "speaker_protocol.h"

#include <Network.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <cstdlib>

// Defined in network_connection.ino. Keep credentials/configuration in one
// place while the existing sketch is migrated to the current wire protocol.
extern const char *ssid;
extern const char *password;
extern const char *SERVER_IP;
extern const uint16_t SERVER_PORT;


bool load_or_create_identity(DeviceIdentity &identity) {
  // Wi-Fi가 시작된 후 호출: ESP32 RNG의 엔트로피 조건을 충족.
  Preferences prefs;
  if (!prefs.begin("speaker", false)) return false;
  if (prefs.isKey("client_id")) {
    identity.client_id = prefs.getULong64("client_id", 0);
    if (identity.client_id == 0) {
      prefs.end();      // 저장값 이상: 조용히 새 ID를 만들지 않음
      return false;
    }
  } else {
    do {
      esp_fill_random(&identity.client_id, sizeof(identity.client_id));
    } while (identity.client_id == 0);

    if (prefs.putULong64("client_id", identity.client_id) != 8) {
      prefs.end();
      return false;
    }
  }

  identity.token = prefs.getULong64("token", 0);
  prefs.end();
  return true;
}

uint32_t next_request_id(uint32_t &sequence) {
  do {
    ++sequence;          // uint32_t의 순환은 정의된 동작
  } while (sequence == SP_NO_REQUEST);  // 0은 사용하지 않음
  return sequence;
}

namespace {

constexpr UBaseType_t kNetworkPriority = 3;
constexpr UBaseType_t kAudioPriority = 4;
constexpr uint32_t kNetworkStackBytes = 6144;
constexpr uint32_t kAudioStackBytes = 4096;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kRetryTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kWifiRetryTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kConnectAckTimeoutTicks = pdMS_TO_TICKS(5000);
constexpr size_t kConnectFrameSize = SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE;
constexpr size_t kNetworkReadChunk = 256;
constexpr int kMaxReadChunksPerLoop = 32;

struct AudioChunk {
  uint8_t *data;
  size_t length;
  uint64_t pts_ms;
};

struct HeartbeatState {
  uint32_t ping_interval_ms = 0;
  uint32_t pong_timeout_ms = 0;
  uint32_t pending_ping_request_id = SP_NO_REQUEST;
  int64_t next_ping_at_us = 0;
  int64_t pong_deadline_us = 0;
  bool active = false;
};

QueueHandle_t command_queue = nullptr;
QueueHandle_t audio_queue = nullptr;
TaskHandle_t network_task_handle = nullptr;
TaskHandle_t audio_task_handle = nullptr;

bool write_all(NetworkClient &client, const uint8_t *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const size_t written = client.write(data + sent, length - sent);
    if (written == 0) return false;
    sent += written;
  }
  return true;
}

bool save_token(DeviceIdentity &identity, uint64_t token) {
  if (token == 0) return false;
  if (token == identity.token) return true;
  Preferences prefs;
  if (!prefs.begin("speaker", false)) return false;
  const bool saved = prefs.putULong64("token", token) == sizeof(token);
  prefs.end();
  if (saved) identity.token = token;
  return saved;
}

void reset_connection_state(SpResponseFrameReceiver &receiver,
                            uint32_t &pending_connect_request_id,
                            bool &ack_received, HeartbeatState &heartbeat) {
  pending_connect_request_id = SP_NO_REQUEST;
  ack_received = false;
  heartbeat = {};
  sp_reset_response_frame(receiver);
}

bool handle_response_frame(const SpResponseFrameReceiver &receiver,
                           uint32_t &pending_connect_request_id,
                           DeviceIdentity &identity, bool &ack_received,
                           bool &should_connect, HeartbeatState &heartbeat) {
  SpResponsePayload payload{};
  const uint8_t *bytes = receiver.header.payload_len == 0
                             ? nullptr : receiver.payload;
  if (sp_parse_response_payload(&receiver.header, bytes,
                                receiver.header.payload_len,
                                &payload) != SpParseStatus::Ok) {
    Serial.println("Invalid server response payload");
    return false;
  }

  if (receiver.header.type == SP_CONNECT_ACK) {
    if (pending_connect_request_id == SP_NO_REQUEST ||
        receiver.header.request_id != pending_connect_request_id) {
      Serial.println("Unexpected CONNECT_ACK request_id");
      return false;
    }
    pending_connect_request_id = SP_NO_REQUEST;
    if (payload.connect_ack.result != 0) {
      Serial.println("Server rejected CONNECT_REQ");
      should_connect = false;
      return false;
    }
    if (payload.connect_ack.ping_interval_ms == 0 ||
        payload.connect_ack.pong_timeout_ms == 0) {
      Serial.println("Invalid heartbeat settings in CONNECT_ACK");
      return false;
    }
    if (!save_token(identity, payload.connect_ack.token)) {
      Serial.println("Failed to persist CONNECT_ACK token");
      should_connect = false;
      return false;
    }
    ack_received = true;
    heartbeat.ping_interval_ms = payload.connect_ack.ping_interval_ms;
    heartbeat.pong_timeout_ms = payload.connect_ack.pong_timeout_ms;
    heartbeat.pending_ping_request_id = SP_NO_REQUEST;
    heartbeat.next_ping_at_us =
        esp_timer_get_time() + static_cast<int64_t>(heartbeat.ping_interval_ms) * 1000;
    heartbeat.pong_deadline_us = 0;
    heartbeat.active = true;
    Serial.printf("CONNECT_ACK received (ping=%u ms, pong timeout=%u ms)\n",
                  static_cast<unsigned>(payload.connect_ack.ping_interval_ms),
                  static_cast<unsigned>(payload.connect_ack.pong_timeout_ms));
    return true;
  }

  if (receiver.header.type == SP_PONG) {
    if (!ack_received || !heartbeat.active ||
        heartbeat.pending_ping_request_id == SP_NO_REQUEST ||
        receiver.header.request_id != heartbeat.pending_ping_request_id) {
      Serial.println("Unexpected PONG request_id");
      return false;
    }
    if (esp_timer_get_time() >= heartbeat.pong_deadline_us) {
      Serial.println("PONG arrived after timeout");
      return false;
    }
    heartbeat.pending_ping_request_id = SP_NO_REQUEST;
    heartbeat.pong_deadline_us = 0;
    Serial.printf("PONG received (request_id=%u, stream_state=%u)\n",
                  static_cast<unsigned>(receiver.header.request_id),
                  static_cast<unsigned>(payload.pong.stream_state));
    return true;
  }

  if (receiver.header.type == SP_ERROR) {
    Serial.printf("Server error code=%u\n",
                  static_cast<unsigned>(payload.error.code));
    should_connect = false;
    return false;
  }
  if (!ack_received) {
    Serial.println("Response arrived before CONNECT_ACK");
    return false;
  }

  // Framing remains synchronized even when other messages follow the ACK.
  // Playback and other control messages are integrated in later steps.
  return true;
}

bool receive_available_frames(NetworkClient &client,
                              SpResponseFrameReceiver &receiver,
                              uint32_t &pending_connect_request_id,
                              DeviceIdentity &identity, bool &ack_received,
                              bool &should_connect, HeartbeatState &heartbeat) {
  uint8_t incoming[kNetworkReadChunk];
  for (int chunk = 0; chunk < kMaxReadChunksPerLoop; ++chunk) {
    const int available = client.available();
    if (available <= 0) break;
    const size_t wanted = static_cast<size_t>(available) < sizeof(incoming)
                              ? static_cast<size_t>(available) : sizeof(incoming);
    const int received = client.read(incoming, wanted);
    if (received < 0) return false;
    if (received == 0) break;

    size_t offset = 0;
    while (offset < static_cast<size_t>(received)) {
      size_t consumed = 0;
      const SpFrameFeedResult result = sp_feed_response_bytes(
          receiver, incoming + offset, static_cast<size_t>(received) - offset,
          &consumed);
      offset += consumed;
      if (result == SpFrameFeedResult::Invalid) {
        Serial.println("Invalid server response header");
        return false;
      }
      if (result == SpFrameFeedResult::Complete) {
        if (!handle_response_frame(receiver, pending_connect_request_id,
                                   identity, ack_received, should_connect,
                                   heartbeat)) {
          return false;
        }
        sp_reset_response_frame(receiver);
      }
    }
  }
  return true;
}


void audio_task(void *) {
  AudioChunk chunk{};
  for (;;) {
    if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    // TODO: decode the compressed frame, then feed PCM to I2S DMA.
    // No producer is connected to this queue until the wire parser and
    // decoder are implemented. Do not claim that received audio is playing.
    free(chunk.data);
  }
}

void network_task(void *) {
  NetworkClient client;
  bool should_connect = false;
  TickType_t next_attempt = 0;
  TickType_t next_wifi_attempt = 0;
  uint32_t request_sequence = 0;
  uint32_t pending_connect_request_id = SP_NO_REQUEST;
  TickType_t connect_ack_deadline = 0;
  DeviceIdentity identity{};
  bool ack_received = false;
  HeartbeatState heartbeat{};
  static SpResponseFrameReceiver receiver{};
  sp_reset_response_frame(receiver);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  for (;;) {
    SpeakerCommand command;
    while (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
      switch (command) {
        case SpeakerCommand::Connect:
          should_connect = true;
          next_attempt = 0;
          break;
        case SpeakerCommand::Disconnect:
          should_connect = false;
          reset_connection_state(receiver, pending_connect_request_id,
                                 ack_received, heartbeat);
          client.stop();
          break;
        case SpeakerCommand::Pause:
        case SpeakerCommand::Resume:
          // post_speaker_command() rejects these until protocol integration.
          break;
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      client.stop();
      reset_connection_state(receiver, pending_connect_request_id,
                             ack_received, heartbeat);
      if (next_wifi_attempt == 0 ||
          static_cast<int32_t>(xTaskGetTickCount() - next_wifi_attempt) >= 0) {
        WiFi.reconnect();
        next_wifi_attempt = xTaskGetTickCount() + kWifiRetryTicks;
      }
      vTaskDelay(kPollTicks);
      continue;
    }

    if (client.available() > 0 &&
        !receive_available_frames(client, receiver,
                                  pending_connect_request_id, identity,
                                  ack_received, should_connect, heartbeat)) {
      client.stop();
      reset_connection_state(receiver, pending_connect_request_id,
                             ack_received, heartbeat);
      next_attempt = xTaskGetTickCount() + kRetryTicks;
    }

    if (pending_connect_request_id != SP_NO_REQUEST &&
        static_cast<int32_t>(xTaskGetTickCount() - connect_ack_deadline) >= 0) {
      Serial.println("CONNECT_ACK timeout");
      client.stop();
      reset_connection_state(receiver, pending_connect_request_id,
                             ack_received, heartbeat);
      next_attempt = xTaskGetTickCount() + kRetryTicks;
    }

    const int64_t now_us = esp_timer_get_time();
    if (heartbeat.active &&
        heartbeat.pending_ping_request_id != SP_NO_REQUEST &&
        now_us >= heartbeat.pong_deadline_us) {
      Serial.println("PONG timeout; reconnecting");
      client.stop();
      reset_connection_state(receiver, pending_connect_request_id,
                             ack_received, heartbeat);
      next_attempt = xTaskGetTickCount() + kRetryTicks;
    }

    if (!client.connected() && client.available() == 0) {
      reset_connection_state(receiver, pending_connect_request_id,
                             ack_received, heartbeat);
    }

    if (heartbeat.active && client.connected() &&
        heartbeat.pending_ping_request_id == SP_NO_REQUEST &&
        now_us >= heartbeat.next_ping_at_us) {
      const uint32_t request_id = next_request_id(request_sequence);
      uint8_t ping[SP_HEADER_SIZE];
      if (!sp_encode_ping_request(request_id, ping, sizeof(ping)) ||
          !write_all(client, ping, sizeof(ping))) {
        Serial.println("Failed to send PING; reconnecting");
        client.stop();
        reset_connection_state(receiver, pending_connect_request_id,
                               ack_received, heartbeat);
        next_attempt = xTaskGetTickCount() + kRetryTicks;
      } else {
        const int64_t sent_at_us = esp_timer_get_time();
        heartbeat.pending_ping_request_id = request_id;
        heartbeat.next_ping_at_us =
            sent_at_us + static_cast<int64_t>(heartbeat.ping_interval_ms) * 1000;
        heartbeat.pong_deadline_us =
            sent_at_us + static_cast<int64_t>(heartbeat.pong_timeout_ms) * 1000;
        Serial.printf("PING sent (request_id=%u)\n",
                      static_cast<unsigned>(request_id));
      }
    }

    if (should_connect && !client.connected() &&
        (next_attempt == 0 ||
         static_cast<int32_t>(xTaskGetTickCount() - next_attempt) >= 0)) {
      client.stop();
      client.setTimeout(1000);
      if (client.connect(SERVER_IP, SERVER_PORT)) {
        identity = {};
        if (!load_or_create_identity(identity)) {
          Serial.println("Failed to load or save client identity");
          client.stop();
        } else {
          const uint32_t request_id = next_request_id(request_sequence);
          const SpConnectRequest request = {identity.client_id, identity.token};
          uint8_t frame[kConnectFrameSize];
          if (!sp_encode_connect_request(request, request_id, frame,
                                         sizeof(frame)) ||
              !write_all(client, frame, sizeof(frame))) {
            Serial.println("Failed to send CONNECT_REQ");
            client.stop();
          } else {
            pending_connect_request_id = request_id;
            connect_ack_deadline = xTaskGetTickCount() + kConnectAckTimeoutTicks;
            Serial.printf("CONNECT_REQ sent (request_id=%u)\n",
                          static_cast<unsigned>(pending_connect_request_id));
          }
        }
      }
      next_attempt = xTaskGetTickCount() + kRetryTicks;
    }

    // Polling sleeps between checks; no timer callback touches the socket.
    // A select()-based wait can replace this when BLE wakeups are integrated.
    vTaskDelay(kPollTicks);
  }
}

}  // namespace

bool start_speaker_tasks() {
  if (command_queue != nullptr || audio_queue != nullptr) return false;

  command_queue = xQueueCreate(8, sizeof(SpeakerCommand));
  audio_queue = xQueueCreate(4, sizeof(AudioChunk));
  if (command_queue == nullptr || audio_queue == nullptr) {
    if (command_queue != nullptr) vQueueDelete(command_queue);
    if (audio_queue != nullptr) vQueueDelete(audio_queue);
    command_queue = nullptr;
    audio_queue = nullptr;
    return false;
  }

  if (xTaskCreate(audio_task, "speaker_audio", kAudioStackBytes, nullptr,
                  kAudioPriority, &audio_task_handle) != pdPASS) {
    vQueueDelete(command_queue);
    vQueueDelete(audio_queue);
    command_queue = nullptr;
    audio_queue = nullptr;
    return false;
  }

  if (xTaskCreate(network_task, "speaker_network", kNetworkStackBytes, nullptr,
                  kNetworkPriority, &network_task_handle) != pdPASS) {
    vTaskDelete(audio_task_handle);
    audio_task_handle = nullptr;
    vQueueDelete(command_queue);
    vQueueDelete(audio_queue);
    command_queue = nullptr;
    audio_queue = nullptr;
    return false;
  }
  return true;
}

bool post_speaker_command(SpeakerCommand command) {
  // Never acknowledge a controller request that the firmware would drop.
  if (command == SpeakerCommand::Pause || command == SpeakerCommand::Resume) {
    return false;
  }
  return command_queue != nullptr &&
         xQueueSend(command_queue, &command, 0) == pdTRUE;
}
