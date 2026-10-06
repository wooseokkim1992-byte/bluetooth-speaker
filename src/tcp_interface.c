#include "tcp_interface.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <stdio.h>

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
    if (buf_size != SP_HEADER_SIZE)
    {
        perror("inproper buf size for header\n");
        return -1;
    }
    header->magic = ntohl(*(uint32_t *)(buf));
    header->version = *(uint8_t *)(buf + 4);
    header->type = *(uint8_t *)(buf + 5);
    header->flags = ntohs(*(uint16_t *)(buf + 6));
    header->request_id = ntohl(*(uint32_t *)(buf + 8));
    header->payload_len = ntohl(*(uint32_t *)(buf + 12));
    return 0;
}

int8_t parsing_payload_connect_REQ(SpHeader *header, void *data, size_t data_len, SpConnectRequest *req)
{
    if (header->payload_len != 1)
    {
        perror("invalid payload length");
        return -1;
    }
    if (SP_HEADER_SIZE + header->payload_len != data_len)
    {
        perror("in proper buffer length\n");
        return -1;
    }
    uint8_t *data_handler = (uint8_t *)data;
    req->client_id_utf8 = *(uint8_t *)(data_handler + header->payload_len + SP_HEADER_SIZE);
    return 0;
}

int8_t construct_header(SpHeader *header, char *buf, size_t buf_size)
{
    if (buf_size != SP_HEADER_SIZE)
    {
        perror("invalid header length\n");
        return -1;
    }
    *(uint32_t *)(buf) = htonl(header->magic);
    *(uint8_t *)(buf + 4) = header->version;
    *(uint8_t *)(buf + 5) = header->type;
    *(uint16_t *)(buf + 6) = htons(header->flags);
    *(uint32_t *)(buf + 8) = htonl(header->request_id);
    *(uint32_t *)(buf + 12) = htonl(header->payload_len);
    return 0;
}

int8_t construct_connect_payload(SpHeader *header, SpConnectAck *payload, char *buf, size_t buf_size)
{
    if (buf_size > SP_MAX_PAYLOAD || buf_size != header->payload_len)
    {
        perror("in proper buf size for payload\n");
        return -1;
    }
    *(uint8_t *)(buf) = payload->result;
    *(uint8_t *)(buf + 1) = payload->plan;
    *(uint8_t *)(buf + 2) = payload->codec;
    *(uint32_t *)(buf + 3) = htonl(payload->bitrate_bps);
    *(uint32_t *)(buf + 7) = htonl(payload->sample_rate_hz);
    *(uint8_t *)(buf + 11) = payload->channels;
    *(uint64_t *)(buf + 12) = hton64((uint8_t *)&(payload->live_pts_ms));
    *(uint16_t *)(buf + 20) = htons(payload->ping_interval_ms);
    *(uint16_t *)(buf + 22) = htons(payload->pong_timeout_ms);
    return 0;
}
