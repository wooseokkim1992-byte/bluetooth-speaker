#ifndef SPEAKER_WEB_HTTP_H
#define SPEAKER_WEB_HTTP_H
#include <stddef.h>

#define WEB_BODY_MAX 4096
typedef struct {
    char method[8], path[256];
    char host[128], origin[256], cookie[1024], content_type[128];
    char body[WEB_BODY_MAX + 1];
    size_t body_length;
} WebRequest;
typedef struct {
    char login_id[401], password[513], confirm[513];
    char client_id[21];
} WebForm;

/* 0 on success, HTTP error code on malformed/oversized/incomplete input. */
int web_read_request(int fd, WebRequest *out);
int web_parse_form(const WebRequest *request, WebForm *out);
int web_same_origin(const WebRequest *request);
void web_reply(int fd, int status, const char *type, const char *body,
                const char *set_cookie);
int web_cookie_token(const char *cookie, char out[65]);
#endif
