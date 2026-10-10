#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../header/tcp_interface.h"

enum class SpParseStatus : uint8_t {
  Ok,
  InvalidArgument,
  InvalidMagic,
  UnsupportedVersion,
  UnsupportedType,
  InvalidFlags,
  InvalidLength,
  InvalidValue,
};

// Only the member selected by header.type is populated. For AUDIO_DATA,
// NOW_PLAYING and ERROR, pointer fields refer to the caller's payload buffer;
// keep that buffer alive until the data has been consumed or copied.
struct SpResponsePayload {
  SpConnectAck connect_ack;
  SpPong pong;
  SpPauseAck pause_ack;
  SpResumeAck resume_ack;
  SpNowPlaying now_playing;
  SpAudioData audio_data;
  SpError error;
};

// data must contain exactly the 16-byte wire header. Integer fields on the
// wire are big-endian; no wire bytes are cast to C/C++ structs.
SpParseStatus sp_parse_response_header(const uint8_t *data, size_t length,
                                       SpHeader *out);

// Call only after a complete payload_len bytes have been assembled from TCP.
// A null payload is valid only for the zero-length DISCONNECT_ACK payload.
SpParseStatus sp_parse_response_payload(const SpHeader *header,
                                        const uint8_t *payload, size_t length,
                                        SpResponsePayload *out);

enum class SpFrameFeedResult : uint8_t {
  NeedMore,
  Complete,
  Invalid,
};

// Persistent receive state for one TCP connection. Keep this off a small task
// stack: payload alone can be SP_MAX_PAYLOAD bytes.
struct SpResponseFrameReceiver {
  uint8_t header_bytes[SP_HEADER_SIZE];
  uint8_t payload[SP_MAX_PAYLOAD];
  SpHeader header;
  size_t header_received;
  size_t payload_received;
  bool complete;
};

void sp_reset_response_frame(SpResponseFrameReceiver &receiver);
SpFrameFeedResult sp_feed_response_bytes(SpResponseFrameReceiver &receiver,
                                         const uint8_t *data, size_t length,
                                         size_t *consumed);

void put_u32_be(uint8_t *p, uint32_t v);
void put_u64_be(uint8_t *p, uint64_t v);

// Builds the complete 16-byte header + 16-byte CONNECT_REQ payload.
// request_id and client_id must be nonzero; out must have room for 32 bytes.
bool sp_encode_connect_request(const SpConnectRequest &request,
                               uint32_t request_id, uint8_t *out,
                               size_t out_size);

// PING has only a 16-byte header and no payload.
bool sp_encode_ping_request(uint32_t request_id, uint8_t *out,
                            size_t out_size);

// Encodes a header-only PAUSE_REQ, RESUME_REQ or DISCONNECT_REQ.
bool sp_encode_control_request(SpMessageType type, uint32_t request_id,
                               uint8_t *out, size_t out_size);
