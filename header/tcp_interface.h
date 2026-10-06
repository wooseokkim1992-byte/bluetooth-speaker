#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define SP_HEADER_SIZE 16u
#define SP_MAX_PAYLOAD 24u
#define SP_MAGIC UINT32_C(0x53504B32) /* SPK2 */
#define SP_VERSION 2u
#define SP_NO_REQUEST UINT32_C(0)
#define SP_NO_SESSION UINT64_C(0)
typedef enum
{
	SP_CONNECT_REQ = 0x01,
	SP_CONNECT_ACK = 0x02,
	SP_PING = 0x03,
	SP_PONG = 0x04,
	SP_DISCONNECT_REQ = 0x05,
	SP_DISCONNECT_ACK = 0x06,
	SP_PAUSE_REQ = 0x10,
	SP_PAUSE_ACK = 0x11,
	SP_RESUME_REQ = 0x12,
	SP_RESUME_ACK = 0x13,
	SP_NOW_PLAYING = 0x20,
	SP_AUDIO_DATA = 0x21,
	SP_PLAYBACK_REPORT = 0x22,
	SP_ERROR = 0x7F
} SpMessageType;
typedef struct
{
	uint32_t magic;
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint32_t request_id;
	uint32_t payload_len;
} SpHeader; /* wire header is always 24 bytes */
typedef struct
{
	uint8_t client_id_utf8;
} SpConnectRequest;
typedef struct
{
	uint8_t result;
	uint8_t plan;  /* 1=Base, 2=Premium */
	uint8_t codec; /* 1=MP3 */
	uint32_t bitrate_bps;
	uint32_t sample_rate_hz;
	uint8_t channels;
	uint64_t live_pts_ms;
	uint16_t ping_interval_ms;
	uint16_t pong_timeout_ms;
} SpConnectAck; /* wire payload length = 24 */
typedef struct
{
	uint64_t live_pts_ms;
	uint8_t stream_state;
} SpPong; /* wire payload length = 9 */
typedef struct
{
	uint64_t stream_pts_ms;
	const uint8_t *mp3_data;
	uint32_t mp3_len; /* <= SP_MAX_PAYLOAD - 8 */
} SpAudioData;
typedef struct
{
	uint64_t last_played_pts_ms;
	uint32_t buffer_ms;
} SpPlaybackReport; /* wire payload length = 12 */
typedef struct
{
	uint64_t track_id;
	uint16_t title_len;
	uint16_t artist_len;
	const uint8_t *title_utf8;
	const uint8_t *artist_utf8;
} SpNowPlaying;
typedef struct
{
	uint16_t code;
	uint16_t detail_len;
	const uint8_t *detail_utf8;
} SpError;

ssize_t write_all(int fd, const void *buf, size_t total);

ssize_t read_all(int fd, const void *buf, size_t total);

int8_t parsing_header(SpHeader *header, const char *buf, size_t buf_size);

uint64_t ntoh64(const uint8_t *buf);
uint64_t hton64(const uint8_t *buf);

int8_t parsing_payload_connect_REQ(SpHeader *header, void *data, size_t data_len, SpConnectRequest *req);

int8_t construct_header(SpHeader *header, char *buf, size_t buf_size);
int8_t construct_connect_payload(SpHeader *header, SpConnectAck *payload, char *buf, size_t buf_size);
