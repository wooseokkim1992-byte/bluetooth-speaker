#include "speaker_protocol.h"

#include <cstring>

namespace {

uint16_t read_u16_be(const uint8_t *data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t read_u32_be(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint64_t read_u64_be(const uint8_t *data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

SpParseStatus validate_response_header(const SpHeader &header) {
  if (header.magic != SP_MAGIC) return SpParseStatus::InvalidMagic;
  if (header.version != SP_VERSION) return SpParseStatus::UnsupportedVersion;
  if (header.flags != 0) return SpParseStatus::InvalidFlags;
  if (header.payload_len > SP_MAX_PAYLOAD) return SpParseStatus::InvalidLength;

  switch (header.type) {
    case SP_CONNECT_ACK:
      return header.payload_len == SP_CONNECT_ACK_PAYLOAD_SIZE
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_PONG:
      return header.payload_len == SP_PONG_PAYLOAD_SIZE
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_DISCONNECT_ACK:
      return header.payload_len == 0
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_PAUSE_ACK:
      return header.payload_len == SP_PAUSE_ACK_PAYLOAD_SIZE
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_RESUME_ACK:
      return header.payload_len == SP_RESUME_ACK_PAYLOAD_SIZE
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_NOW_PLAYING:
      return header.payload_len >= SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_AUDIO_DATA:
      return header.payload_len >= sizeof(uint64_t)
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    case SP_ERROR:
      return header.payload_len >= 4
                 ? SpParseStatus::Ok : SpParseStatus::InvalidLength;
    default:
      return SpParseStatus::UnsupportedType;
  }
}

}  // namespace

void put_u32_be(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void put_u64_be(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    p[i] = uint8_t(v >> (56 - 8 * i));
}

bool sp_encode_connect_request(const SpConnectRequest &request,
                               uint32_t request_id, uint8_t *out,
                               size_t out_size) {
  constexpr size_t kFrameSize = SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE;
  if (out == nullptr || out_size < kFrameSize ||
      request_id == SP_NO_REQUEST || request.client_id_utf8 == 0) {
    return false;
  }

  put_u32_be(out, SP_MAGIC);
  out[4] = SP_VERSION;
  out[5] = SP_CONNECT_REQ;
  out[6] = 0;
  out[7] = 0;
  put_u32_be(out + 8, request_id);
  put_u32_be(out + 12, SP_CONNECT_REQ_PAYLOAD_SIZE);
  put_u64_be(out + SP_HEADER_SIZE, request.client_id_utf8);
  put_u64_be(out + SP_HEADER_SIZE + 8, request.token);
  return true;
}

bool sp_encode_ping_request(uint32_t request_id, uint8_t *out,
                            size_t out_size) {
  if (out == nullptr || out_size < SP_HEADER_SIZE ||
      request_id == SP_NO_REQUEST) {
    return false;
  }
  put_u32_be(out, SP_MAGIC);
  out[4] = SP_VERSION;
  out[5] = SP_PING;
  out[6] = 0;
  out[7] = 0;
  put_u32_be(out + 8, request_id);
  put_u32_be(out + 12, 0);
  return true;
}

void sp_reset_response_frame(SpResponseFrameReceiver &receiver) {
  receiver.header_received = 0;
  receiver.payload_received = 0;
  receiver.complete = false;
  receiver.header = {};
}

SpFrameFeedResult sp_feed_response_bytes(SpResponseFrameReceiver &receiver,
                                         const uint8_t *data, size_t length,
                                         size_t *consumed) {
  if (consumed == nullptr || (length != 0 && data == nullptr)) {
    return SpFrameFeedResult::Invalid;
  }
  *consumed = 0;
  if (receiver.complete) return SpFrameFeedResult::Complete;

  while (*consumed < length) {
    if (receiver.header_received < SP_HEADER_SIZE) {
      const size_t remaining = SP_HEADER_SIZE - receiver.header_received;
      const size_t available = length - *consumed;
      const size_t n = remaining < available ? remaining : available;
      memcpy(receiver.header_bytes + receiver.header_received,
             data + *consumed, n);
      receiver.header_received += n;
      *consumed += n;
      if (receiver.header_received < SP_HEADER_SIZE) continue;

      if (sp_parse_response_header(receiver.header_bytes, SP_HEADER_SIZE,
                                   &receiver.header) != SpParseStatus::Ok) {
        return SpFrameFeedResult::Invalid;
      }
      if (receiver.header.payload_len == 0) {
        receiver.complete = true;
        return SpFrameFeedResult::Complete;
      }
    }

    const size_t remaining = receiver.header.payload_len - receiver.payload_received;
    const size_t available = length - *consumed;
    const size_t n = remaining < available ? remaining : available;
    if (n != 0) {
      memcpy(receiver.payload + receiver.payload_received, data + *consumed, n);
      receiver.payload_received += n;
      *consumed += n;
    }
    if (receiver.payload_received == receiver.header.payload_len) {
      receiver.complete = true;
      return SpFrameFeedResult::Complete;
    }
  }
  return SpFrameFeedResult::NeedMore;
}

SpParseStatus sp_parse_response_header(const uint8_t *data, size_t length,
                                       SpHeader *out) {
  if (data == nullptr || out == nullptr) return SpParseStatus::InvalidArgument;
  if (length != SP_HEADER_SIZE) return SpParseStatus::InvalidLength;

  SpHeader parsed{};
  parsed.magic = read_u32_be(data);
  parsed.version = data[4];
  parsed.type = data[5];
  parsed.flags = read_u16_be(data + 6);
  parsed.request_id = read_u32_be(data + 8);
  parsed.payload_len = read_u32_be(data + 12);

  const SpParseStatus status = validate_response_header(parsed);
  if (status == SpParseStatus::Ok) *out = parsed;
  return status;
}

SpParseStatus sp_parse_response_payload(const SpHeader *header,
                                        const uint8_t *payload, size_t length,
                                        SpResponsePayload *out) {
  if (header == nullptr || out == nullptr ||
      (length != 0 && payload == nullptr)) {
    return SpParseStatus::InvalidArgument;
  }
  const SpParseStatus header_status = validate_response_header(*header);
  if (header_status != SpParseStatus::Ok) return header_status;
  if (length != header->payload_len) return SpParseStatus::InvalidLength;

  SpResponsePayload parsed{};
  switch (header->type) {
    case SP_CONNECT_ACK:
      parsed.connect_ack.result = payload[0];
      parsed.connect_ack.plan = payload[1];
      parsed.connect_ack.codec = payload[2];
      parsed.connect_ack.bitrate_bps = read_u32_be(payload + 3);
      parsed.connect_ack.sample_rate_hz = read_u32_be(payload + 7);
      parsed.connect_ack.channels = payload[11];
      parsed.connect_ack.live_pts_ms = read_u64_be(payload + 12);
      parsed.connect_ack.ping_interval_ms = read_u16_be(payload + 20);
      parsed.connect_ack.pong_timeout_ms = read_u16_be(payload + 22);
      parsed.connect_ack.token = read_u64_be(payload + 24);
      break;
    case SP_PONG:
      parsed.pong.live_pts_ms = read_u64_be(payload);
      parsed.pong.stream_state = payload[8];
      if (parsed.pong.stream_state > 1) return SpParseStatus::InvalidValue;
      break;
    case SP_DISCONNECT_ACK:
      break;
    case SP_PAUSE_ACK:
      parsed.pause_ack.result = payload[0];
      break;
    case SP_RESUME_ACK:
      parsed.resume_ack.result = payload[0];
      parsed.resume_ack.live_pts_ms = read_u64_be(payload + 1);
      break;
    case SP_NOW_PLAYING:
      parsed.now_playing.track_id = read_u64_be(payload);
      parsed.now_playing.title_len = read_u16_be(payload + 8);
      if (parsed.now_playing.title_len !=
          length - SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE) {
        return SpParseStatus::InvalidLength;
      }
      parsed.now_playing.title_utf8 = parsed.now_playing.title_len == 0
                                          ? nullptr
                                          : payload + SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE;
      break;
    case SP_AUDIO_DATA:
      parsed.audio_data.stream_pts_ms = read_u64_be(payload);
      parsed.audio_data.data_len = static_cast<uint32_t>(length - sizeof(uint64_t));
      parsed.audio_data.data = parsed.audio_data.data_len == 0
                                       ? nullptr : payload + sizeof(uint64_t);
      break;
    case SP_ERROR:
      parsed.error.code = read_u16_be(payload);
      parsed.error.detail_len = read_u16_be(payload + 2);
      if (parsed.error.detail_len != length - 4) {
        return SpParseStatus::InvalidLength;
      }
      parsed.error.detail_utf8 = parsed.error.detail_len == 0
                                     ? nullptr : payload + 4;
      break;
    default:
      return SpParseStatus::UnsupportedType;
  }

  *out = parsed;
  return SpParseStatus::Ok;
}
