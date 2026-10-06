#include "tcp_interface.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_header_and_connect_request(void)
{
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_CONNECT_REQ, 0, 42, 1};
    uint8_t frame[SP_HEADER_SIZE + 1] = {0};
    assert(construct_header(&header, (char *)frame, SP_HEADER_SIZE) == 0);
    frame[SP_HEADER_SIZE] = 0x7a;

    SpHeader parsed = {0};
    SpConnectRequest request = {0};
    assert(parsing_header(&parsed, (const char *)frame, SP_HEADER_SIZE) == 0);
    assert(parsed.magic == SP_MAGIC && parsed.version == SP_VERSION);
    assert(parsed.type == SP_CONNECT_REQ && parsed.request_id == 42);
    assert(parsed.payload_len == 1);
    assert(parsing_payload_connect_REQ(&parsed, frame, sizeof(frame), &request) == 0);
    assert(request.client_id_utf8 == 0x7a);
}

static void test_connect_ack(void)
{
    SpHeader header = {SP_MAGIC, SP_VERSION, SP_CONNECT_ACK, 0, 42,
                       SP_CONNECT_ACK_PAYLOAD_SIZE};
    SpConnectAck payload = {0, 1, 1, 128000, 44100, 2,
                            UINT64_C(0x0102030405060708), 1000, 10000};
    const uint8_t expected[SP_CONNECT_ACK_PAYLOAD_SIZE] = {
        0, 1, 1, 0, 1, 0xf4, 0, 0, 0, 0xac, 0x44, 2,
        1, 2, 3, 4, 5, 6, 7, 8, 0x03, 0xe8, 0x27, 0x10
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

int main(void)
{
    test_header_and_connect_request();
    test_connect_ack();
    test_pong_pause_resume();
    test_now_playing();
    return 0;
}
