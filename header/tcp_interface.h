#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define SP_HEADER_SIZE 16u
#define SP_MAX_PAYLOAD 4096u
#define SP_CONNECT_REQ_PAYLOAD_SIZE 16u
#define SP_CONNECT_ACK_PAYLOAD_SIZE 32u
#define SP_PONG_PAYLOAD_SIZE 9u
#define SP_PAUSE_ACK_PAYLOAD_SIZE 1u
#define SP_RESUME_ACK_PAYLOAD_SIZE 9u
#define SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE 10u
#define SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE 8u
#define SP_MAGIC UINT32_C(0x53504B32) /* SPK2 */
#define SP_VERSION 2u
#define SP_NO_REQUEST UINT32_C(0)
#define SP_NO_SESSION UINT64_C(0)
#define SP_CODEC_MP3 1u
#define SP_CODEC_PCM_S16LE 2u
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
} SpHeader; /* wire header is always 16 bytes */
typedef struct
{
	uint64_t client_id_utf8;
	uint64_t token;
} SpConnectRequest;
typedef struct
{
	uint8_t result;
	uint8_t plan;  /* 1=Base, 2=Premium */
	uint8_t codec; /* 1=MP3, 2=interleaved 16-bit little-endian stereo PCM */
	uint32_t bitrate_bps;
	uint32_t sample_rate_hz;
	uint8_t channels;
	uint64_t live_pts_ms;
	uint16_t ping_interval_ms;
	uint16_t pong_timeout_ms;
	uint64_t token;
} SpConnectAck; /* wire payload length = 32 */
typedef struct
{
	uint64_t live_pts_ms;
	uint8_t stream_state;
} SpPong; /* wire payload length = 9 */
typedef struct
{
	uint64_t stream_pts_ms;
	const uint8_t *data;
	uint32_t data_len; /* <= SP_MAX_PAYLOAD - 8; codec from CONNECT_ACK */
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
	const uint8_t *title_utf8;
} SpNowPlaying;

typedef struct
{
	uint8_t result; // 0:success 1:fail
} SpPauseAck;

typedef struct
{
	uint8_t result;
	uint64_t live_pts_ms;
} SpResumeAck;

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
int8_t construct_connect_request_payload(SpHeader *header, SpConnectRequest *payload, char *buf, size_t buf_size);
int8_t construct_connect_payload(SpHeader *header, SpConnectAck *payload, char *buf, size_t buf_size);
int8_t construct_pong_payload(SpHeader *header, SpPong *resp_data, char *buf, size_t buf_size);
int8_t construct_resume_payload(SpHeader *header, SpResumeAck *resp_data, char *buf, size_t buf_size);
int8_t construct_pause_payload(SpHeader *header, SpPauseAck *resp_data, char *buf, size_t buf_size);
int8_t construct_Now_Playing_payload(SpHeader *header, SpNowPlaying *resp_data, char *buf, size_t buf_size);
int8_t construct_audio_payload(SpHeader *header, const SpAudioData *audio, char *buf, size_t buf_size);
int8_t construct_error_payload(SpHeader *header, SpError *resp_data, char *buf);
int8_t generate_token(uint64_t *token);
int8_t check_magic_num(SpHeader *header);
