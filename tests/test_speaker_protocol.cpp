#include "../esp32/music_controller_ino/speaker_protocol.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {

std::array<uint8_t, SP_HEADER_SIZE> wire_header(uint8_t type,
                                                 uint32_t payload_len) {
  return {{'S', 'P', 'K', '2', SP_VERSION, type, 0, 0,
           0, 0, 0, 7,
           static_cast<uint8_t>(payload_len >> 24),
           static_cast<uint8_t>(payload_len >> 16),
           static_cast<uint8_t>(payload_len >> 8),
           static_cast<uint8_t>(payload_len)}};
}

SpHeader parse_header(uint8_t type, uint32_t payload_len) {
  auto bytes = wire_header(type, payload_len);
  SpHeader header{};
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::Ok);
  assert(header.magic == SP_MAGIC);
  assert(header.version == SP_VERSION);
  assert(header.type == type);
  assert(header.request_id == 7);
  assert(header.payload_len == payload_len);
  return header;
}

void test_headers() {
  auto bytes = wire_header(SP_CONNECT_ACK, SP_CONNECT_ACK_PAYLOAD_SIZE);
  SpHeader header{};
  assert(sp_parse_response_header(bytes.data(), 15, &header) ==
         SpParseStatus::InvalidLength);
  bytes[0] = 'X';
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::InvalidMagic);
  bytes[0] = 'S';
  bytes[4] = 99;
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::UnsupportedVersion);
  bytes[4] = SP_VERSION;
  bytes[6] = 1;
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::InvalidFlags);
  bytes[6] = 0;
  bytes[5] = SP_PING;  // A client request cannot be parsed as a response.
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::UnsupportedType);
  bytes = wire_header(SP_AUDIO_DATA, SP_MAX_PAYLOAD + 1);
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::InvalidLength);
  bytes = wire_header(SP_NOW_PLAYING, 9);
  assert(sp_parse_response_header(bytes.data(), bytes.size(), &header) ==
         SpParseStatus::InvalidLength);
}

void test_connect_ack() {
  const SpHeader header = parse_header(SP_CONNECT_ACK, SP_CONNECT_ACK_PAYLOAD_SIZE);
  const uint8_t payload[SP_CONNECT_ACK_PAYLOAD_SIZE] = {
      0, 1, 1, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x00, 0xac, 0x44, 2,
      1, 2, 3, 4, 5, 6, 7, 8, 0x03, 0xe8, 0x27, 0x10,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  SpResponsePayload parsed{};
  assert(sp_parse_response_payload(&header, payload, sizeof(payload), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.connect_ack.plan == 1);
  assert(parsed.connect_ack.codec == 1);
  assert(parsed.connect_ack.bitrate_bps == 128000);
  assert(parsed.connect_ack.sample_rate_hz == 44100);
  assert(parsed.connect_ack.channels == 2);
  assert(parsed.connect_ack.live_pts_ms == UINT64_C(0x0102030405060708));
  assert(parsed.connect_ack.ping_interval_ms == 1000);
  assert(parsed.connect_ack.pong_timeout_ms == 10000);
  assert(parsed.connect_ack.token == UINT64_C(0x1122334455667788));
  assert(sp_parse_response_payload(&header, payload, sizeof(payload) - 1,
                                   &parsed) ==
         SpParseStatus::InvalidLength);
}

void test_connect_request_encoding() {
  const SpConnectRequest request = {
      UINT64_C(0x0102030405060708), UINT64_C(0x1122334455667788)};
  uint8_t frame[SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE] = {};
  assert(sp_encode_connect_request(request, 7, frame, sizeof(frame)));
  const uint8_t expected[sizeof(frame)] = {
      'S', 'P', 'K', '2', SP_VERSION, SP_CONNECT_REQ, 0, 0,
      0, 0, 0, 7, 0, 0, 0, SP_CONNECT_REQ_PAYLOAD_SIZE,
      1, 2, 3, 4, 5, 6, 7, 8,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  for (size_t i = 0; i < sizeof(frame); ++i) assert(frame[i] == expected[i]);
  assert(!sp_encode_connect_request(request, SP_NO_REQUEST, frame,
                                    sizeof(frame)));
  assert(!sp_encode_connect_request(request, 7, frame, sizeof(frame) - 1));
}

void test_ping_request_encoding() {
  uint8_t frame[SP_HEADER_SIZE] = {};
  assert(sp_encode_ping_request(0x01020304, frame, sizeof(frame)));
  const uint8_t expected[SP_HEADER_SIZE] = {
      'S', 'P', 'K', '2', SP_VERSION, SP_PING, 0, 0,
      1, 2, 3, 4, 0, 0, 0, 0};
  for (size_t i = 0; i < sizeof(frame); ++i) assert(frame[i] == expected[i]);
  assert(!sp_encode_ping_request(SP_NO_REQUEST, frame, sizeof(frame)));
  assert(!sp_encode_ping_request(1, frame, sizeof(frame) - 1));
}

void test_fragmented_and_coalesced_responses() {
  constexpr size_t kAckFrameSize = SP_HEADER_SIZE + SP_CONNECT_ACK_PAYLOAD_SIZE;
  constexpr size_t kPongFrameSize = SP_HEADER_SIZE + SP_PONG_PAYLOAD_SIZE;
  uint8_t wire[kAckFrameSize + kPongFrameSize] = {};
  const auto ack_header = wire_header(SP_CONNECT_ACK, SP_CONNECT_ACK_PAYLOAD_SIZE);
  const auto pong_header = wire_header(SP_PONG, SP_PONG_PAYLOAD_SIZE);
  memcpy(wire, ack_header.data(), SP_HEADER_SIZE);
  wire[SP_HEADER_SIZE + 31] = 9;  // ACK token = 9
  memcpy(wire + kAckFrameSize, pong_header.data(), SP_HEADER_SIZE);
  wire[kAckFrameSize + SP_HEADER_SIZE + 8] = 1;  // PLAYING

  SpResponseFrameReceiver receiver{};
  sp_reset_response_frame(receiver);
  size_t consumed = 0;
  assert(sp_feed_response_bytes(receiver, wire, 7, &consumed) ==
         SpFrameFeedResult::NeedMore);
  assert(consumed == 7);
  assert(sp_feed_response_bytes(receiver, wire + 7, sizeof(wire) - 7,
                                &consumed) == SpFrameFeedResult::Complete);
  assert(consumed == kAckFrameSize - 7);
  assert(receiver.header.type == SP_CONNECT_ACK);
  SpResponsePayload payload{};
  assert(sp_parse_response_payload(&receiver.header, receiver.payload,
                                   receiver.header.payload_len,
                                   &payload) == SpParseStatus::Ok);
  assert(payload.connect_ack.token == 9);

  sp_reset_response_frame(receiver);
  assert(sp_feed_response_bytes(receiver, wire + kAckFrameSize,
                                kPongFrameSize, &consumed) ==
         SpFrameFeedResult::Complete);
  assert(consumed == kPongFrameSize);
  assert(receiver.header.type == SP_PONG);
  assert(receiver.payload[8] == 1);
}

void test_other_responses() {
  SpResponsePayload parsed{};

  auto header = parse_header(SP_PONG, 9);
  const uint8_t pong[9] = {0, 0, 0, 0, 0, 0, 0, 42, 1};
  assert(sp_parse_response_payload(&header, pong, sizeof(pong), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.pong.live_pts_ms == 42);
  assert(parsed.pong.stream_state == 1);
  const uint8_t bad_pong[9] = {0, 0, 0, 0, 0, 0, 0, 42, 2};
  assert(sp_parse_response_payload(&header, bad_pong, sizeof(bad_pong), &parsed) ==
         SpParseStatus::InvalidValue);

  header = parse_header(SP_DISCONNECT_ACK, 0);
  assert(sp_parse_response_payload(&header, nullptr, 0, &parsed) ==
         SpParseStatus::Ok);

  header = parse_header(SP_PAUSE_ACK, 1);
  const uint8_t pause[1] = {0};
  assert(sp_parse_response_payload(&header, pause, sizeof(pause), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.pause_ack.result == 0);

  header = parse_header(SP_RESUME_ACK, 9);
  const uint8_t resume[9] = {0, 0, 0, 0, 0, 0, 0, 0, 99};
  assert(sp_parse_response_payload(&header, resume, sizeof(resume), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.resume_ack.live_pts_ms == 99);

  header = parse_header(SP_NOW_PLAYING, 13);
  const uint8_t now_playing[13] = {0, 0, 0, 0, 0, 0, 0, 5, 0, 3,
                                    0xea, 0xb0, 0x80};  // UTF-8 "가"
  assert(sp_parse_response_payload(&header, now_playing,
                                   sizeof(now_playing), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.now_playing.track_id == 5);
  assert(parsed.now_playing.title_len == 3);
  assert(parsed.now_playing.title_utf8 == now_playing + 10);
  const uint8_t bad_title[13] = {0, 0, 0, 0, 0, 0, 0, 5, 0, 4, 1, 2, 3};
  assert(sp_parse_response_payload(&header, bad_title,
                                   sizeof(bad_title), &parsed) ==
         SpParseStatus::InvalidLength);

  header = parse_header(SP_AUDIO_DATA, 11);
  const uint8_t audio[11] = {0, 0, 0, 0, 0, 0, 0, 77, 0xff, 0xfb, 0x90};
  assert(sp_parse_response_payload(&header, audio, sizeof(audio), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.audio_data.stream_pts_ms == 77);
  assert(parsed.audio_data.mp3_len == 3);
  assert(parsed.audio_data.mp3_data == audio + 8);

  header = parse_header(SP_ERROR, 6);
  const uint8_t error[6] = {0, 9, 0, 2, 'N', 'O'};
  assert(sp_parse_response_payload(&header, error, sizeof(error), &parsed) ==
         SpParseStatus::Ok);
  assert(parsed.error.code == 9);
  assert(parsed.error.detail_len == 2);
  assert(parsed.error.detail_utf8 == error + 4);
}

}  // namespace

int main() {
  test_headers();
  test_connect_ack();
  test_connect_request_encoding();
  test_ping_request_encoding();
  test_fragmented_and_coalesced_responses();
  test_other_responses();
}
