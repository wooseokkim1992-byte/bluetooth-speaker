#include "tcp_interface.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_header_and_connect_request(void)
{
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_CONNECT_REQ, 0, 42,
                       SP_CONNECT_REQ_PAYLOAD_SIZE};
    SpConnectRequest request_data = {
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718),
    };
    uint8_t frame[SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE] = {0};
    assert(construct_header(&header, (char *)frame, SP_HEADER_SIZE) == 0);
    assert(construct_connect_request_payload(&header, &request_data,
                                             (char *)frame + SP_HEADER_SIZE,
                                             SP_CONNECT_REQ_PAYLOAD_SIZE) == 0);
    const uint8_t expected_payload[SP_CONNECT_REQ_PAYLOAD_SIZE] = {
        1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18
    };
    assert(memcmp(frame + SP_HEADER_SIZE, expected_payload,
                  sizeof(expected_payload)) == 0);

    SpHeader parsed = {0};
    SpConnectRequest request = {0};
    assert(parsing_header(&parsed, (const char *)frame, SP_HEADER_SIZE) == 0);
    assert(parsed.magic == SP_MAGIC && parsed.version == SP_VERSION);
    assert(parsed.type == SP_CONNECT_REQ && parsed.request_id == 42);
    assert(parsed.payload_len == SP_CONNECT_REQ_PAYLOAD_SIZE);
    assert(parsing_payload_connect_REQ(&parsed, frame, sizeof(frame), &request) == 0);
    assert(request.client_id_utf8 == request_data.client_id_utf8);
    assert(request.token == request_data.token);
    assert(parsing_payload_connect_REQ(&parsed, frame, sizeof(frame) - 1,
                                       &request) == -1);
}

static void test_connect_ack(void)
{
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_CONNECT_ACK, 0, 42,
                       SP_CONNECT_ACK_PAYLOAD_SIZE};
    SpConnectAck payload = {0, 1, 1, 128000, 44100, 2,
                            UINT64_C(0x0102030405060708), 1000, 10000,
                            UINT64_C(0x1112131415161718)};
    const uint8_t expected[SP_CONNECT_ACK_PAYLOAD_SIZE] = {
        0, 1, 1, 0, 1, 0xf4, 0, 0, 0, 0xac, 0x44, 2,
        1, 2, 3, 4, 5, 6, 7, 8, 0x03, 0xe8, 0x27, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18
    };
    uint8_t wire[sizeof(expected)] = {0};
    assert(construct_connect_payload(&header, &payload, (char *)wire,
                                     sizeof(wire)) == 0);
    assert(memcmp(wire, expected, sizeof(wire)) == 0);
}

static void test_pong_pause_resume(void)
{
    uint8_t wire[SP_RESUME_ACK_PAYLOAD_SIZE] = {0};
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_PONG, 0, 3,
                       SP_PONG_PAYLOAD_SIZE};
    SpPong pong = {UINT64_C(0x0102030405060708), 1};
    const uint8_t pong_expected[SP_PONG_PAYLOAD_SIZE] =
        {1, 2, 3, 4, 5, 6, 7, 8, 1};
    assert(construct_pong_payload(&header, &pong, (char *)wire,
                                  sizeof(wire)) == 0);
    assert(memcmp(wire, pong_expected, sizeof(wire)) == 0);
    pong.stream_state = 2;
    assert(construct_pong_payload(&header, &pong, (char *)wire,
                                  sizeof(wire)) == -1);

    header.type = SP_PAUSE_ACK;
    header.payload_len = SP_PAUSE_ACK_PAYLOAD_SIZE;
    SpPauseAck pause = {0};
    assert(construct_pause_payload(&header, &pause, (char *)wire, 1) == 0);
    assert(wire[0] == 0);

    header.type = SP_RESUME_ACK;
    header.payload_len = SP_RESUME_ACK_PAYLOAD_SIZE;
    SpResumeAck resume = {1, UINT64_C(0x0102030405060708)};
    const uint8_t resume_expected[SP_RESUME_ACK_PAYLOAD_SIZE] =
        {1, 1, 2, 3, 4, 5, 6, 7, 8};
    assert(construct_resume_payload(&header, &resume, (char *)wire,
                                    sizeof(wire)) == 0);
    assert(memcmp(wire, resume_expected, sizeof(wire)) == 0);
    assert(construct_resume_payload(&header, &resume, (char *)wire, 8) == -1);
}

static void test_now_playing(void)
{
    const uint8_t title[] = {0xea, 0xb0, 0x80, 0xeb, 0x82, 0x98};
    SpNowPlaying payload = {UINT64_C(0x0102030405060708), sizeof(title), title};
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_NOW_PLAYING, 0, 0,
                       SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE + sizeof(title)};
    uint8_t wire[SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE + sizeof(title)] = {0};
    const uint8_t expected[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 0, 6,
        0xea, 0xb0, 0x80, 0xeb, 0x82, 0x98
    };
    assert(construct_Now_Playing_payload(&header, &payload, (char *)wire,
                                         sizeof(wire)) == 0);
    assert(memcmp(wire, expected, sizeof(wire)) == 0);
    header.payload_len--;
    assert(construct_Now_Playing_payload(&header, &payload, (char *)wire,
                                         sizeof(wire)) == -1);
}

static void test_audio_payload(void)
{
    const uint8_t pcm[] = {0x01, 0x02, 0x03, 0x04,
                           0x05, 0x06, 0x07, 0x08};
    SpAudioData audio = {
        .stream_pts_ms = UINT64_C(0x0102030405060708),
        .data = pcm,
        .data_len = sizeof(pcm),
    };
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_AUDIO_DATA, 0, 0,
                       SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE + sizeof(pcm)};
    const uint8_t expected[] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };
    uint8_t wire[sizeof(expected)] = {0};
    assert(construct_audio_payload(&header, &audio, (char *)wire,
                                   sizeof(wire)) == 0);
    assert(memcmp(wire, expected, sizeof(wire)) == 0);
    assert(construct_audio_payload(&header, &audio, (char *)wire,
                                   sizeof(wire) - 1) == -1);
}

static void test_generate_token(void)
{
    uint64_t token = 0;
    assert(generate_token(&token) == 0);
    assert(token != 0);
    assert(generate_token(NULL) == -1);
}

int main(void)
{
    test_header_and_connect_request();
    test_connect_ack();
    test_pong_pause_resume();
    test_now_playing();
    test_audio_payload();
    test_generate_token();
    return 0;
}
