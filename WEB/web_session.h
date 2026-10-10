#ifndef SPEAKER_WEB_SESSION_H
#define SPEAKER_WEB_SESSION_H
#include <stdint.h>
typedef struct { uint64_t member_id; } WebSession;
int web_session_create(uint64_t member_id, char token[65]);
int web_session_find(const char *token, WebSession *out);
void web_session_remove(const char *token);
#endif
