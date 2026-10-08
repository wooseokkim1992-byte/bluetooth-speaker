#include "speaker_tasks.h"
#include "speaker_protocol.h"
#include "pcm_ring.h"

#include <Network.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/i2s_std.h>
#include <esp_timer.h>

#include <cstring>

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
constexpr uint32_t kAudioStackBytes = 6144;
// 44.1 kHz x 16-bit x 2 channels = 176400 bytes/s. A 50 ms poll with the
// old 8 KiB read cap could not keep up with a raw PCM stream.
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr TickType_t kRetryTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kWifiRetryTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kConnectAckTimeoutTicks = pdMS_TO_TICKS(5000);
constexpr size_t kConnectFrameSize = SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE;
constexpr size_t kNetworkReadChunk = 1024;
constexpr int kMaxReadChunksPerLoop = 32;
constexpr uint32_t kPcmSampleRate = 44100;
constexpr size_t kPcmFrameBytes = 4;  // 16-bit left + 16-bit right.
constexpr size_t kPcmRingBytes = 32768;
constexpr size_t kPcmPrebufferBytes = 8192;
constexpr size_t kI2sWriteBytes = 1024;
constexpr gpio_num_t kI2sBclkPin = GPIO_NUM_26;
constexpr gpio_num_t kI2sWsPin = GPIO_NUM_25;
constexpr gpio_num_t kI2sDataPin = GPIO_NUM_22;
constexpr size_t kMaxTitleBytes = SP_MAX_PAYLOAD - SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE;

struct HeartbeatState {
  uint32_t ping_interval_ms = 0;
  uint32_t pong_timeout_ms = 0;
  uint32_t pending_ping_request_id = SP_NO_REQUEST;
  int64_t next_ping_at_us = 0;
  int64_t pong_deadline_us = 0;
  bool active = false;
};

QueueHandle_t command_queue = nullptr;
TaskHandle_t network_task_handle = nullptr;
TaskHandle_t audio_task_handle = nullptr;
SemaphoreHandle_t audio_mutex = nullptr;
SemaphoreHandle_t metadata_mutex = nullptr;
i2s_chan_handle_t i2s_tx = nullptr;
uint8_t pcm_storage[kPcmRingBytes];
PcmRingBuffer pcm_ring(pcm_storage, sizeof(pcm_storage));
bool audio_accepting = false;
bool audio_primed = false;
uint32_t audio_generation = 0;
uint64_t newest_audio_pts_ms = 0;
uint64_t current_track_id = 0;
char current_title[kMaxTitleBytes + 1] = {};
size_t current_title_len = 0;
bool now_playing_valid = false;

bool init_i2s_tx() {
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = 6;
  channel_config.dma_frame_num = 128;
  if (i2s_new_channel(&channel_config, &i2s_tx, nullptr) != ESP_OK) {
    return false;
  }

  i2s_std_config_t config{};
  const i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(kPcmSampleRate);
  const i2s_std_slot_config_t slot =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                          I2S_SLOT_MODE_STEREO);
  config.clk_cfg = clock;
  config.slot_cfg = slot;
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // MAX98357A does not need MCLK.
  config.gpio_cfg.bclk = kI2sBclkPin;
  config.gpio_cfg.ws = kI2sWsPin;
  config.gpio_cfg.dout = kI2sDataPin;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(i2s_tx, &config) != ESP_OK) {
    i2s_del_channel(i2s_tx);
    i2s_tx = nullptr;
    return false;
  }
  return true;
}

void set_audio_accepting(bool accepting) {
  xSemaphoreTake(audio_mutex, portMAX_DELAY);
  bool changed = false;
  if (audio_accepting != accepting || (!accepting && pcm_ring.size() != 0)) {
    pcm_ring.clear();
    audio_primed = false;
    audio_accepting = accepting;
    newest_audio_pts_ms = 0;
    ++audio_generation;
    changed = true;
  }
  xSemaphoreGive(audio_mutex);
  if (changed && audio_task_handle != nullptr) xTaskNotifyGive(audio_task_handle);
}

void clear_now_playing() {
  xSemaphoreTake(metadata_mutex, portMAX_DELAY);
  current_track_id = 0;
  current_title[0] = '\0';
  current_title_len = 0;
  now_playing_valid = false;
  xSemaphoreGive(metadata_mutex);
}

bool update_now_playing(const SpNowPlaying &now_playing) {
  xSemaphoreTake(metadata_mutex, portMAX_DELAY);
  const bool changed = !now_playing_valid ||
                       current_track_id != now_playing.track_id;
  current_track_id = now_playing.track_id;
  current_title_len = now_playing.title_len;
  if (current_title_len != 0) {
    memcpy(current_title, now_playing.title_utf8, current_title_len);
  }
  current_title[current_title_len] = '\0';
  now_playing_valid = true;
  xSemaphoreGive(metadata_mutex);
  return changed;
}

bool enqueue_pcm(const SpAudioData &audio) {
  if (audio.data == nullptr || audio.data_len == 0 ||
      audio.data_len % kPcmFrameBytes != 0 ||
      audio.data_len > kPcmRingBytes) {
    return false;
  }
  xSemaphoreTake(audio_mutex, portMAX_DELAY);
  if (!audio_accepting) {
    xSemaphoreGive(audio_mutex);
    return true;  // A stopped broadcast may still have an in-flight frame.
  }
  bool dropped_stale = false;
  if (audio.data_len > pcm_ring.free_space() ||
      (newest_audio_pts_ms != 0 &&
       (audio.stream_pts_ms < newest_audio_pts_ms ||
        audio.stream_pts_ms - newest_audio_pts_ms > 1000))) {
    // Never play stale buffered audio after a slow consumer/producer burst.
    pcm_ring.clear();
    audio_primed = false;
    ++audio_generation;
    dropped_stale = true;
  }
  const bool written = pcm_ring.write(audio.data, audio.data_len);
  if (written) newest_audio_pts_ms = audio.stream_pts_ms;
  xSemaphoreGive(audio_mutex);
  if (dropped_stale) Serial.println("PCM buffer reset: overflow or PTS jump");
  if (written && audio_task_handle != nullptr) xTaskNotifyGive(audio_task_handle);
  return written;
}

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
  set_audio_accepting(false);
  clear_now_playing();
}

bool handle_response_frame(const SpResponseFrameReceiver &receiver,
                           uint32_t &pending_connect_request_id,
                           DeviceIdentity &identity, bool &ack_received,
                           bool &should_connect, HeartbeatState &heartbeat,
                           bool &pcm_profile_supported) {
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
    pcm_profile_supported =
        payload.connect_ack.codec == SP_CODEC_PCM_S16LE &&
        payload.connect_ack.sample_rate_hz == kPcmSampleRate &&
        payload.connect_ack.channels == 2;
    set_audio_accepting(pcm_profile_supported);
    if (!pcm_profile_supported) {
      Serial.printf("Unsupported audio profile: codec=%u rate=%u channels=%u; "
                    "control connection remains active\n",
                    static_cast<unsigned>(payload.connect_ack.codec),
                    static_cast<unsigned>(payload.connect_ack.sample_rate_hz),
                    static_cast<unsigned>(payload.connect_ack.channels));
    }
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
    set_audio_accepting(pcm_profile_supported &&
                        payload.pong.stream_state == 1);
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

  if (receiver.header.type == SP_NOW_PLAYING) {
    if (receiver.header.request_id != SP_NO_REQUEST) return false;
    const bool changed = update_now_playing(payload.now_playing);
    if (changed) {
      // A new track must not play PCM left over from the previous track.
      set_audio_accepting(false);
      set_audio_accepting(pcm_profile_supported);
    }
    Serial.printf("NOW_PLAYING track=%llu title_bytes=%u\n",
                  static_cast<unsigned long long>(payload.now_playing.track_id),
                  static_cast<unsigned>(payload.now_playing.title_len));
    return true;
  }

  if (receiver.header.type == SP_AUDIO_DATA) {
    if (receiver.header.request_id != SP_NO_REQUEST) return false;
    if (!pcm_profile_supported) return true;  // Never send MP3 bytes to I2S.
    if (!enqueue_pcm(payload.audio_data)) {
      Serial.println("Invalid or unqueueable PCM AUDIO_DATA");
      return false;
    }
    return true;
  }

  return true;
}

bool receive_available_frames(NetworkClient &client,
                              SpResponseFrameReceiver &receiver,
                              uint32_t &pending_connect_request_id,
                              DeviceIdentity &identity, bool &ack_received,
                              bool &should_connect, HeartbeatState &heartbeat,
                              bool &pcm_profile_supported) {
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
                                   heartbeat, pcm_profile_supported)) {
          return false;
        }
        sp_reset_response_frame(receiver);
      }
    }
  }
  return true;
}


void audio_task(void *) {
  uint8_t output[kI2sWriteBytes];
  bool tx_enabled = false;
  uint32_t seen_generation = 0;
  for (;;) {
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    const uint32_t generation = audio_generation;
    const bool accepting = audio_accepting;
    if (accepting && !audio_primed &&
        pcm_ring.size() >= kPcmPrebufferBytes) {
      audio_primed = true;
    }
    const size_t read_size = accepting && audio_primed
                                 ? pcm_ring.read(output, sizeof(output)) : 0;
    if (accepting && audio_primed && read_size == 0) audio_primed = false;
    xSemaphoreGive(audio_mutex);

    if (generation != seen_generation || !accepting || read_size == 0) {
      if (tx_enabled) {
        i2s_channel_disable(i2s_tx);
        tx_enabled = false;
      }
      seen_generation = generation;
    }
    if (!accepting || read_size == 0) {
      // A PAUSE, disconnect or underrun stops DMA output. Wait for new PCM.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }

    // Drop bytes copied just before a network-side reset/track change.
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    const bool still_current =
        audio_accepting && audio_generation == generation;
    xSemaphoreGive(audio_mutex);
    if (!still_current) continue;

    if (!tx_enabled) {
      if (i2s_channel_enable(i2s_tx) != ESP_OK) {
        Serial.println("I2S TX enable failed");
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      tx_enabled = true;
    }
    size_t sent = 0;
    while (sent < read_size) {
      size_t written = 0;
      const esp_err_t result = i2s_channel_write(
          i2s_tx, output + sent, read_size - sent, &written, 20);
      sent += written;
      if (result != ESP_OK || written == 0) {
        Serial.printf("I2S TX write failed: %d\n", static_cast<int>(result));
        i2s_channel_disable(i2s_tx);
        tx_enabled = false;
        break;
      }
    }
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
  bool pcm_profile_supported = false;
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
                                  ack_received, should_connect, heartbeat,
                                  pcm_profile_supported)) {
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
  if (command_queue != nullptr || audio_mutex != nullptr ||
      metadata_mutex != nullptr || i2s_tx != nullptr) return false;

  command_queue = xQueueCreate(8, sizeof(SpeakerCommand));
  audio_mutex = xSemaphoreCreateMutex();
  metadata_mutex = xSemaphoreCreateMutex();
  if (command_queue == nullptr || audio_mutex == nullptr ||
      metadata_mutex == nullptr || !init_i2s_tx()) {
    if (command_queue != nullptr) vQueueDelete(command_queue);
    if (audio_mutex != nullptr) vSemaphoreDelete(audio_mutex);
    if (metadata_mutex != nullptr) vSemaphoreDelete(metadata_mutex);
    command_queue = nullptr;
    audio_mutex = nullptr;
    metadata_mutex = nullptr;
    Serial.println("Failed to prepare I2S audio pipeline");
    return false;
  }

  if (xTaskCreate(audio_task, "speaker_audio", kAudioStackBytes, nullptr,
                  kAudioPriority, &audio_task_handle) != pdPASS) {
    vQueueDelete(command_queue);
    vSemaphoreDelete(audio_mutex);
    vSemaphoreDelete(metadata_mutex);
    i2s_del_channel(i2s_tx);
    command_queue = nullptr;
    audio_mutex = nullptr;
    metadata_mutex = nullptr;
    i2s_tx = nullptr;
    return false;
  }

  if (xTaskCreate(network_task, "speaker_network", kNetworkStackBytes, nullptr,
                  kNetworkPriority, &network_task_handle) != pdPASS) {
    vTaskDelete(audio_task_handle);
    audio_task_handle = nullptr;
    vQueueDelete(command_queue);
    vSemaphoreDelete(audio_mutex);
    vSemaphoreDelete(metadata_mutex);
    i2s_del_channel(i2s_tx);
    command_queue = nullptr;
    audio_mutex = nullptr;
    metadata_mutex = nullptr;
    i2s_tx = nullptr;
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

bool get_speaker_now_playing(uint64_t *track_id, char *title,
                             size_t title_capacity) {
  if (track_id == nullptr || title == nullptr || title_capacity == 0 ||
      metadata_mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(metadata_mutex, portMAX_DELAY);
  const bool valid = now_playing_valid;
  if (valid) {
    *track_id = current_track_id;
    size_t copied = current_title_len < title_capacity - 1
                        ? current_title_len : title_capacity - 1;
    if (copied < current_title_len) {
      // Do not terminate a truncated UTF-8 title inside a code point.
      while (copied > 0 &&
             (static_cast<uint8_t>(current_title[copied]) & 0xc0) == 0x80) {
        --copied;
      }
    }
    if (copied != 0) memcpy(title, current_title, copied);
    title[copied] = '\0';
  }
  xSemaphoreGive(metadata_mutex);
  return valid;
}
