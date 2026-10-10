#define _GNU_SOURCE
#include "web_session.h"
#include <errno.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

/* Single-process, sequential request handling. Sessions expire in one hour. */
static struct {
    char token[65];
    WebSession user;
    time_t expires;
} sessions[64];

int web_session_create(uint64_t member_id, char token[65])
{
    time_t now = time(NULL);
    size_t slot;
    for (slot = 0; slot < 64; ++slot) if (sessions[slot].expires <= now) break;
    if (slot == 64) return -1;
    unsigned char random[32];
    size_t used = 0;
    while (used < sizeof(random)) {
        ssize_t n = getrandom(random + used, sizeof(random) - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        token[2 * i] = digits[random[i] >> 4];
        token[2 * i + 1] = digits[random[i] & 15];
    }
    token[64] = '\0';
    strcpy(sessions[slot].token, token);
    sessions[slot].user = (WebSession){member_id};
    sessions[slot].expires = now + 3600;
    explicit_bzero(random, sizeof(random));
    return 0;
}

int web_session_find(const char *token, WebSession *out)
{
    if (!token || !*token) return 0;
    for (size_t i = 0; i < 64; ++i) {
        if (sessions[i].expires > time(NULL) && !strcmp(sessions[i].token, token)) {
            *out = sessions[i].user;
            return 1;
        }
    }
    return 0;
}

void web_session_remove(const char *token)
{
    if (!token || !*token) return;
    for (size_t i = 0; i < 64; ++i)
        if (!strcmp(sessions[i].token, token)) explicit_bzero(&sessions[i], sizeof(sessions[i]));
}
