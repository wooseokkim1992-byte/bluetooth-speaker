#include "tcp_interface.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/random.h>

static uint16_t read_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static uint32_t read_u32_be(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static void write_u16_be(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value >> 8);
    buf[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *buf, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        buf[i] = (uint8_t)(value >> (56 - 8 * i));
}

static int8_t check_payload_args(const SpHeader *header, const void *payload,
                                 const char *buf, size_t buf_size,
                                 SpMessageType type, size_t payload_size)
{
    if (header == NULL || payload == NULL || buf == NULL ||
        header->type != type || payload_size > SP_MAX_PAYLOAD ||
        header->payload_len != payload_size || buf_size < payload_size)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

ssize_t write_all(int fd, const void *buf, size_t total)
{
    if (total > SSIZE_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    const unsigned char *bytes = buf;
    size_t offset = 0;

    while (offset < total)
    {
        ssize_t written = write(fd, bytes + offset, total - offset);
        if (written > 0)
        {
            offset += (size_t)written;
        }
        else if (written < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            if (written == 0)
                errno = EIO;
            return -1;
        }
    }
    return (ssize_t)offset;
}

ssize_t read_all(int fd, const void *buf, size_t total)
{
    if (total > SSIZE_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    size_t read_byte = 0;
    unsigned char *data_buf = (unsigned char *)buf;
    while (read_byte < total)
    {
        ssize_t n = read(fd, data_buf + read_byte, total - read_byte);
        if (n > 0)
        {
            read_byte += (size_t)n;
        }
        else
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
    }
    return (ssize_t)read_byte;
}

uint64_t ntoh64(const uint8_t *buf)
{
    return ((uint64_t)buf[0] << 56) |
           ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) |
           ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) |
           ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8) |
           (uint64_t)buf[7];
}

uint64_t hton64(const uint8_t *buf)
{
    return ((uint64_t)buf[7]) |
           ((uint64_t)buf[6] << 8) |
           ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[4] << 24) |
           ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[2] << 40) |
           ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[0] << 56);
}

int8_t parsing_header(SpHeader *header, const char *buf, size_t buf_size)
{
    if (header == NULL || buf == NULL || buf_size != SP_HEADER_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)buf;
    header->magic = read_u32_be(bytes);
    header->version = bytes[4];
    header->type = bytes[5];
    header->flags = read_u16_be(bytes + 6);
    header->request_id = read_u32_be(bytes + 8);
    header->payload_len = read_u32_be(bytes + 12);
    return 0;
}

int8_t parsing_payload_connect_REQ(SpHeader *header, void *data, size_t data_len, SpConnectRequest *req)
{
    if (header == NULL || data == NULL || req == NULL ||
        header->type != SP_CONNECT_REQ ||
        header->payload_len != SP_CONNECT_REQ_PAYLOAD_SIZE ||
        data_len != SP_HEADER_SIZE + SP_CONNECT_REQ_PAYLOAD_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    const uint8_t *payload = (const uint8_t *)data + SP_HEADER_SIZE;
    req->client_id_utf8 = ntoh64(payload);
    req->token = ntoh64(payload + 8);
    return 0;
}

int8_t construct_header(SpHeader *header, char *buf, size_t buf_size)
{
    if (header == NULL || buf == NULL || buf_size != SP_HEADER_SIZE ||
        header->payload_len > SP_MAX_PAYLOAD)
    {
        errno = EINVAL;
        return -1;
    }
    uint8_t *bytes = (uint8_t *)buf;
    write_u32_be(bytes, header->magic);
    bytes[4] = header->version;
    bytes[5] = header->type;
    write_u16_be(bytes + 6, header->flags);
    write_u32_be(bytes + 8, header->request_id);
    write_u32_be(bytes + 12, header->payload_len);
    return 0;
}

int8_t construct_connect_request_payload(SpHeader *header,
                                         SpConnectRequest *payload,
                                         char *buf, size_t buf_size)
{
    if (check_payload_args(header, payload, buf, buf_size, SP_CONNECT_REQ,
                           SP_CONNECT_REQ_PAYLOAD_SIZE) != 0)
        return -1;
    uint8_t *bytes = (uint8_t *)buf;
    write_u64_be(bytes, payload->client_id_utf8);
    write_u64_be(bytes + 8, payload->token);
    return 0;
}

int8_t construct_connect_payload(SpHeader *header, SpConnectAck *payload, char *buf, size_t buf_size)
{
    if (check_payload_args(header, payload, buf, buf_size, SP_CONNECT_ACK,
                           SP_CONNECT_ACK_PAYLOAD_SIZE) != 0)
        return -1;
    uint8_t *bytes = (uint8_t *)buf;
    bytes[0] = payload->result;
    bytes[1] = payload->plan;
    bytes[2] = payload->codec;
    write_u32_be(bytes + 3, payload->bitrate_bps);
    write_u32_be(bytes + 7, payload->sample_rate_hz);
    bytes[11] = payload->channels;
    write_u64_be(bytes + 12, payload->live_pts_ms);
    write_u16_be(bytes + 20, payload->ping_interval_ms);
    write_u16_be(bytes + 22, payload->pong_timeout_ms);
    write_u64_be(bytes + 24, payload->token);
    return 0;
}

int8_t construct_pong_payload(SpHeader *header, SpPong *resp_data, char *buf, size_t buf_size)
{
    if (check_payload_args(header, resp_data, buf, buf_size, SP_PONG,
                           SP_PONG_PAYLOAD_SIZE) != 0)
        return -1;
    if (resp_data->stream_state > 1)
    {
        errno = EINVAL;
        return -1;
    }
    uint8_t *bytes = (uint8_t *)buf;
    write_u64_be(bytes, resp_data->live_pts_ms);
    bytes[8] = resp_data->stream_state;
    return 0;
}

int8_t construct_resume_payload(SpHeader *header, SpResumeAck *resp_data, char *buf, size_t buf_size)
{
    if (check_payload_args(header, resp_data, buf, buf_size, SP_RESUME_ACK,
                           SP_RESUME_ACK_PAYLOAD_SIZE) != 0)
        return -1;
    uint8_t *bytes = (uint8_t *)buf;
    bytes[0] = resp_data->result;
    write_u64_be(bytes + 1, resp_data->live_pts_ms);
    return 0;
}

int8_t construct_pause_payload(SpHeader *header, SpPauseAck *resp_data, char *buf, size_t buf_size)
{
    if (check_payload_args(header, resp_data, buf, buf_size, SP_PAUSE_ACK,
                           SP_PAUSE_ACK_PAYLOAD_SIZE) != 0)
        return -1;
    ((uint8_t *)buf)[0] = resp_data->result;
    return 0;
}

int8_t construct_Now_Playing_payload(SpHeader *header, SpNowPlaying *resp_data, char *buf, size_t buf_size)
{
    if (header == NULL || resp_data == NULL ||
        resp_data->title_len > SP_MAX_PAYLOAD - SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    size_t payload_size = SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE + resp_data->title_len;
    if (check_payload_args(header, resp_data, buf, buf_size, SP_NOW_PLAYING,
                           payload_size) != 0 ||
        (resp_data->title_len != 0 && resp_data->title_utf8 == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    uint8_t *bytes = (uint8_t *)buf;
    write_u64_be(bytes, resp_data->track_id);
    write_u16_be(bytes + 8, resp_data->title_len);
    if (resp_data->title_len != 0)
        memcpy(bytes + SP_NOW_PLAYING_FIXED_PAYLOAD_SIZE,
               resp_data->title_utf8, resp_data->title_len);
    return 0;
}

int8_t construct_audio_payload(SpHeader *header, const SpAudioData *audio,
                               char *buf, size_t buf_size)
{
    if (audio == NULL || audio->data == NULL || audio->data_len == 0 ||
        audio->data_len > SP_MAX_PAYLOAD - SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    size_t payload_size = SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE + audio->data_len;
    if (check_payload_args(header, audio, buf, buf_size, SP_AUDIO_DATA,
                           payload_size) != 0)
        return -1;
    write_u64_be((uint8_t *)buf, audio->stream_pts_ms);
    memcpy(buf + SP_AUDIO_DATA_FIXED_PAYLOAD_SIZE, audio->data,
           audio->data_len);
    return 0;
}

int8_t construct_error_payload(SpHeader *header, SpError *resp_data, char *buf)
{
    if (!header || !resp_data || !buf)
    {
        perror("not enough objs\n");
        return -2;
    }
    if (header->payload_len > SP_MAX_PAYLOAD)
    {
        perror("not a proper size\n");
        return -1;
    }
    write_u16_be((uint8_t *)buf, resp_data->code);
    memcpy(buf + 2, resp_data->detail_utf8, resp_data->detail_len);
    return 0;
}

int8_t generate_token(uint64_t *token)
{
    if (token == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    uint64_t val;
    do
    {
        size_t received = 0;
        while (received < sizeof(val))
        {
            ssize_t n = getrandom((unsigned char *)&val + received, sizeof(val) - received, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (n == 0)
            {
                errno = EIO;
                return -1;
            }
            received += (size_t)n;
        }
    } while (val == 0);
    *token = val;
    return 0;
}

int8_t check_magic_num(SpHeader *header)
{
    if (header == NULL)
    {
        perror("no header obj");
        return -1;
    }
    uint8_t *magic_hex = (uint8_t *)(&header->magic);
    uint32_t original_magic = (uint32_t)SP_MAGIC;
    uint8_t *original_magic_bytes = (uint8_t *)&original_magic;
    for (int8_t i = 0; i < 4; i++)
    {
        uint8_t result = magic_hex[i] ^ original_magic_bytes[i];
        if (result)
        {
            return -1;
        }
    }
    return 0;
}
