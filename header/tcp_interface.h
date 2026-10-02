#pragma once

#include <stdint.h>
#include <stddef.h>

#define SP_HEADER_SIZE  24u
#define SP_MAX_PAYLOAD  4096u
#define SP_MAGIC        UINT32_C(0x53504B31) /* "SPK1" */
#define SP_VERSION      1u
#define SP_ASYNC_REQUEST_ID 0u
#define SP_NO_SESSION_ID    UINT64_C(0)

/* enum의 sizeof는 컴파일러 의존적이므로 wire의 type은 uint8_t */
typedef enum {
    SP_HELLO            = 0x01,
    SP_HELLO_ACK        = 0x02,
    SP_PING             = 0x03,
    SP_PONG             = 0x04,
    SP_LIST_REQUEST     = 0x10,
    SP_LIST_RESPONSE    = 0x11,
    SP_PLAY_REQUEST     = 0x20,
    SP_PLAY_ACCEPTED    = 0x21,
    SP_PAUSE            = 0x22,
    SP_RESUME           = 0x23,
    SP_STOP             = 0x24,
    SP_COMMAND_ACCEPTED = 0x25,
    SP_STATUS           = 0x30,
    SP_ERROR            = 0x31
} SpMessageType;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t request_id;
    uint32_t payload_len;
    uint64_t session_id;
} SpHeader;  /* wire 크기는 항상 24; sizeof(SpHeader)에 의존 금지 */

/* 고정 길이 payload의 논리적 표현 */
typedef struct { uint8_t device_id[16]; } SpHelloPayload;
typedef struct {
    uint32_t ping_interval_ms;
    uint32_t pong_timeout_ms;
} SpHelloAckPayload;
typedef struct { uint64_t track_id; } SpPlayRequestPayload;

typedef enum {
    SP_STATE_IDLE = 0,
    SP_STATE_STARTING = 1,
    SP_STATE_PLAYING = 2,
    SP_STATE_PAUSED = 3,
    SP_STATE_STOPPING = 4,
    SP_STATE_STOPPED = 5,
    SP_STATE_ERROR = 6
} SpPlaybackState;

typedef struct {
    uint8_t state;          /* wire: 1 byte */
    uint32_t position_ms;   /* wire: 바로 뒤 4 bytes; padding 없음 */
} SpStatusPayload;          /* wire 크기 5; sizeof에 의존 금지 */

/* 가변 길이 payload: 문자열은 NUL 없이 길이+바이트로 전송 */
typedef struct {
    uint64_t track_id;
    uint32_t duration_ms;
    uint16_t title_len;
    const uint8_t *title_utf8;   /* title_len bytes */
} SpTrackInfo;

typedef struct {
    uint32_t count;
    const SpTrackInfo *tracks;   /* wire에는 포인터가 아닌 항목들이 순서대로 옴 */
} SpListResponsePayload;

typedef struct {
    uint16_t code;
    uint16_t detail_len;
    const uint8_t *detail_utf8;  /* detail_len bytes */
} SpErrorPayload;