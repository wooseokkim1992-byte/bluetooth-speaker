#define _GNU_SOURCE
#include "signup.h"
#include "db_internal.h"
#include <crypt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

static SignupResult fail(SignupInfo *out, SignupResult result, const char *text)
{
    snprintf(out->message, sizeof(out->message), "%s", text);
    return result;
}

SignupResult signup_member(Db *db, const char *login_id,
                           const char *password, SignupInfo *out)
{
    if (!out) return SIGNUP_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    if (!db || !login_id || !*login_id || strlen(login_id) > 100
            || !password || strlen(password) < 8 || strlen(password) > 128)
        return fail(out, SIGNUP_INVALID_INPUT, "Check ID and password length");
    for (const unsigned char *p = (const unsigned char *)login_id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return fail(out, SIGNUP_INVALID_INPUT, "Invalid login ID");

    unsigned char random[16];
    size_t used = 0;
    while (used < sizeof(random)) {
        ssize_t n = getrandom(random + used, sizeof(random) - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(out, SIGNUP_RANDOM_ERROR, "Cannot generate salt");
        used += (size_t)n;
    }
    /* SHA-512 crypt with random salt; uses the Linux crypt library. */
    const char *digits = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char salt[20] = "$6$";
    for (size_t i = 0; i < 16; ++i) salt[3 + i] = digits[random[i] & 63];
    salt[19] = '\0';
    explicit_bzero(random, sizeof(random));
    struct crypt_data *data = calloc(1, sizeof(*data));
    if (!data) return fail(out, SIGNUP_HASH_ERROR, "Cannot allocate hash memory");
    char *hash = crypt_r(password, salt, data);
    if (!hash || hash[0] == '*' || strlen(hash) > 255) {
        explicit_bzero(data, sizeof(*data)); free(data);
        return fail(out, SIGNUP_HASH_ERROR, "Cannot hash password");
    }
    char *id_value = db_text_value(login_id), *hash_value = db_text_value(hash);
    explicit_bzero(data, sizeof(*data)); free(data);
    if (!id_value || !hash_value) {
        free(id_value); free(hash_value);
        return fail(out, SIGNUP_DB_ERROR, "Cannot allocate SQL values");
    }
    DbResult result = db_query(db,
        "INSERT INTO Member(login_id,password_hash) VALUES(%s,%s)", id_value, hash_value);
    unsigned int error = mysql_errno(db->connection);
    free(id_value);
    explicit_bzero(hash_value, strlen(hash_value)); free(hash_value);
    if (result != DB_OK)
        return fail(out, error == 1062 ? SIGNUP_ALREADY_EXISTS : SIGNUP_DB_ERROR, db_error(db));
    out->member_id = (uint64_t)mysql_insert_id(db->connection);
    return fail(out, SIGNUP_OK, "Member registered");
}
