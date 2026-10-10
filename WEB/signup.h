#ifndef SPEAKER_SIGNUP_H
#define SPEAKER_SIGNUP_H

#include "db_api.h"

typedef enum {
    SIGNUP_OK = 0,
    SIGNUP_INVALID_INPUT,
    SIGNUP_ALREADY_EXISTS,
    SIGNUP_RANDOM_ERROR,
    SIGNUP_DB_ERROR,
    SIGNUP_HASH_ERROR
} SignupResult;

typedef struct {
    uint64_t member_id;
    char message[256];
} SignupInfo;

/* Call in a worker with its own Db connection, not the streaming event loop.
 * Login ID: 1..100 ASCII letters/digits or . _ -
 * Password: 8..128 bytes. A salted SHA-512 crypt hash is stored using libcrypt.
 * Caller clears its password buffer after use.
 * Creates only Member. Device registration is separate.
 */
SignupResult signup_member(Db *db,
                           const char *login_id, const char *password,
                           SignupInfo *out);

#endif
