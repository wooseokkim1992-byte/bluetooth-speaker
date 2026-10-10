#define _GNU_SOURCE
#include "login.h"
#include "db_internal.h"
#include <crypt.h>
#include <stdlib.h>
#include <string.h>

LoginResult login_member(Db *db, const char *login_id,
                          const char *password, LoginInfo *out)
{
    if (!out) return LOGIN_INVALID;
    memset(out, 0, sizeof(*out));
    if (!db || !login_id || !*login_id || strlen(login_id) > 100
            || !password || strlen(password) < 8 || strlen(password) > 128)
        return LOGIN_INVALID;
    char *id_value = db_text_value(login_id);
    if (!id_value) return LOGIN_SYSTEM_ERROR;
    DbResult status = db_query(db,
        "SELECT member_id,password_hash FROM Member WHERE login_id=%s", id_value);
    free(id_value);
    if (status != DB_OK) return LOGIN_DB_ERROR;
    MYSQL_RES *result;
    if (db_get_result(db, &result) != DB_OK) return LOGIN_DB_ERROR;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row || !row[0] || !row[1]) {
        mysql_free_result(result); return LOGIN_INVALID;
    }
    uint64_t member_id = (uint64_t)strtoull(row[0], NULL, 10);
    char saved[256];
    if (db_copy_text(saved, sizeof(saved), row[1])) {
        mysql_free_result(result); return LOGIN_DB_ERROR;
    }
    mysql_free_result(result);
    /* Accept existing yescrypt hashes and this app's SHA-512 crypt hashes. */
    if (strncmp(saved, "$y$", 3) && strncmp(saved, "$6$", 3)) {
        explicit_bzero(saved, sizeof(saved)); return LOGIN_INVALID;
    }
    struct crypt_data *data = calloc(1, sizeof(*data));
    if (!data) { explicit_bzero(saved, sizeof(saved)); return LOGIN_SYSTEM_ERROR; }
    char *hash = crypt_r(password, saved, data);
    int match = 0;
    if (hash && hash[0] != '*' && strlen(hash) == strlen(saved)) {
        unsigned char difference = 0;
        for (size_t i = 0; i < strlen(saved); ++i)
            difference |= (unsigned char)hash[i] ^ (unsigned char)saved[i];
        match = difference == 0;
    }
    explicit_bzero(saved, sizeof(saved));
    explicit_bzero(data, sizeof(*data)); free(data);
    if (!match) return LOGIN_INVALID;
    out->member_id = member_id;
    return LOGIN_OK;
}
