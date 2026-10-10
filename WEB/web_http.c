#define _GNU_SOURCE
#include "web_http.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

static int copy(char *out, size_t size, const char *text)
{
    if (strlen(text) >= size) return -1;
    strcpy(out, text);
    return 0;
}

int web_read_request(int fd, WebRequest *out)
{
    char buffer[8192 + WEB_BODY_MAX + 1];
    size_t used = 0, header_size = 0;
    memset(out, 0, sizeof(*out));
    for (;;) {
        if (used >= sizeof(buffer) - 1) return 413;
        ssize_t n = recv(fd, buffer + used, sizeof(buffer) - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 400;
        used += (size_t)n;
        buffer[used] = '\0';
        char *end = strstr(buffer, "\r\n\r\n");
        if (end) { header_size = (size_t)(end - buffer) + 4; break; }
        if (used >= 8192) return 431;
    }
    if (header_size > 8192 || memchr(buffer, '\0', header_size)) return 400;
    buffer[header_size - 2] = '\0';
    char *save, *line = strtok_r(buffer, "\r\n", &save);
    char version[16], extra;
    if (!line || sscanf(line, "%7s %255s %15s %c", out->method, out->path,
                        version, &extra) != 3
            || (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) return 400;
    size_t length = 0;
    int have_length = 0, have_host = 0, have_origin = 0, have_cookie = 0, have_type = 0;
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        char *colon = strchr(line, ':');
        if (!colon || line[0] == ' ' || line[0] == '\t') return 400;
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') ++colon;
        size_t tail = strlen(colon);
        while (tail && (colon[tail - 1] == ' ' || colon[tail - 1] == '\t')) colon[--tail] = '\0';
        for (const unsigned char *p = (const unsigned char *)colon; *p; ++p)
            if (*p < 32 || *p == 127) return 400;
        if (!strcasecmp(line, "Content-Length")) {
            if (have_length++ || !*colon) return 400;
            for (const char *p = colon; *p; ++p) {
                if (*p < '0' || *p > '9') return 400;
                length = length * 10 + (size_t)(*p - '0');
                if (length > WEB_BODY_MAX) return 413;
            }
        } else if (!strcasecmp(line, "Transfer-Encoding")) return 400;
        else if (!strcasecmp(line, "Host")) {
            if (have_host++ || !*colon || copy(out->host, sizeof(out->host), colon)) return 400;
        } else if (!strcasecmp(line, "Origin")) {
            if (have_origin++ || copy(out->origin, sizeof(out->origin), colon)) return 400;
        } else if (!strcasecmp(line, "Cookie")) {
            if (have_cookie++ || copy(out->cookie, sizeof(out->cookie), colon)) return 400;
        } else if (!strcasecmp(line, "Content-Type")) {
            if (have_type++ || copy(out->content_type, sizeof(out->content_type), colon)) return 400;
        }
    }
    if (!have_host || (!strcmp(out->method, "POST") && !have_length)) return 400;
    size_t received = used - header_size;
    if (received > length) received = length;
    memcpy(out->body, buffer + header_size, received);
    while (received < length) {
        ssize_t n = recv(fd, out->body + received, length - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 400;
        received += (size_t)n;
    }
    out->body[length] = '\0';
    out->body_length = length;
    int invalid = memchr(out->body, '\0', length) != NULL;
    explicit_bzero(buffer, sizeof(buffer));
    return invalid ? 400 : 0;
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode(const char *text, char *out, size_t size)
{
    size_t used = 0;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c == '%') {
            if (!text[0] || !text[1] || hex((unsigned char)text[0]) < 0
                    || hex((unsigned char)text[1]) < 0) return -1;
            c = (unsigned char)(hex((unsigned char)text[0]) * 16 + hex((unsigned char)text[1]));
            text += 2;
        } else if (c == '+') c = ' ';
        if (!c || used + 1 >= size) return -1;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return 0;
}

int web_parse_form(const WebRequest *request, WebForm *out)
{
    const char *type = "application/x-www-form-urlencoded";
    size_t type_length = strlen(type);
    if (strncasecmp(request->content_type, type, type_length)
            || (request->content_type[type_length] && request->content_type[type_length] != ';')) return -1;
    char body[WEB_BODY_MAX + 1];
    memcpy(body, request->body, request->body_length + 1);
    memset(out, 0, sizeof(*out));
    unsigned seen = 0;
    int bad = 0;
    char *save, *pair = strtok_r(body, "&", &save);
    for (; pair; pair = strtok_r(NULL, "&", &save)) {
        char *equals = strchr(pair, '=');
        if (!equals) { bad = 1; break; }
        *equals++ = '\0';
        char key[32];
        if (decode(pair, key, sizeof(key))) { bad = 1; break; }
        char *destination;
        size_t capacity;
        unsigned bit;
        if (!strcmp(key, "login_id")) { destination = out->login_id; capacity = sizeof(out->login_id); bit = 1; }
        else if (!strcmp(key, "password")) { destination = out->password; capacity = sizeof(out->password); bit = 2; }
        else if (!strcmp(key, "confirm")) { destination = out->confirm; capacity = sizeof(out->confirm); bit = 4; }
        else if ((!strcmp(key, "client_id") || !strcmp(key, "device_uuid"))) { destination = out->client_id; capacity = sizeof(out->client_id); bit = 8; }
        else { bad = 1; break; }
        if ((seen & bit) || decode(equals, destination, capacity)) { bad = 1; break; }
        seen |= bit;
    }
    explicit_bzero(body, sizeof(body));
    if (!strcmp(request->path, "/api/verify-device") ||
            !strcmp(request->path, "/api/register-device"))
        return bad || seen != 8 ? -1 : 0;
    return bad || (seen & 3) != 3 || (seen & 8) ? -1 : 0;
}

int web_same_origin(const WebRequest *request)
{
    char expected[256];
    snprintf(expected, sizeof(expected), "http://%s", request->host);
    return *request->origin && !strcmp(expected, request->origin);
}

static int send_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t n = send(fd, text, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        text += n;
        length -= (size_t)n;
    }
    return 0;
}

void web_reply(int fd, int status, const char *type, const char *body, const char *set_cookie)
{
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d Response\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n%s%s%s\r\n", status, type, strlen(body),
        set_cookie ? "Set-Cookie: " : "", set_cookie ? set_cookie : "", set_cookie ? "\r\n" : "");
    if (n > 0 && (size_t)n < sizeof(header) && !send_all(fd, header, (size_t)n))
        send_all(fd, body, strlen(body));
}

int web_cookie_token(const char *cookie, char out[65])
{
    char copy_cookie[1024], *save;
    out[0] = '\0';
    if (copy(copy_cookie, sizeof(copy_cookie), cookie)) return 0;
    for (char *item = strtok_r(copy_cookie, ";", &save); item; item = strtok_r(NULL, ";", &save)) {
        while (*item == ' ') ++item;
        if (strncmp(item, "speaker_session=", 16)) continue;
        const char *value = item + 16;
        if (strlen(value) != 64) return 0;
        for (int i = 0; i < 64; ++i) if (!isxdigit((unsigned char)value[i])) return 0;
        strcpy(out, value);
        return 1;
    }
    return 0;
}
