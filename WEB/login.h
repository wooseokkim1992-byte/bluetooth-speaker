#ifndef SPEAKER_LOGIN_H
#define SPEAKER_LOGIN_H
#include "db_api.h"

typedef enum { LOGIN_OK, LOGIN_INVALID, LOGIN_DB_ERROR, LOGIN_SYSTEM_ERROR } LoginResult;
typedef struct {
    uint64_t member_id;
} LoginInfo;

/* Verifies Member credentials only. No automatic Device creation. */
LoginResult login_member(Db *db, const char *login_id, const char *password,
                          LoginInfo *out);
#endif
