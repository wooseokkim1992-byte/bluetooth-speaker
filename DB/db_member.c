#define _GNU_SOURCE
#include "db_internal.h"
#include <mysql.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int valid_uuid(const char *input, char out[37])
{
    if (!input || strlen(input) != 36) return 0;
    for (int i = 0; i < 36; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!isxdigit(c)) return 0;
        out[i] = (char)tolower(c);
    }
    out[36] = '\0';
    return 1;
}

static int valid_login_id(const char *login_id)
{
    return login_id && *login_id && strlen(login_id) <= 400;
}

static DbResult read_account(Db *db, AccountInfo *out)
{
    MYSQL_RES *result;
    DbResult status = db_get_result(db, &result);
    if (status != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return db_set_error(db, DB_NOT_FOUND, "Member not found"); }
    int bad = db_copy_text(out->member_uuid, sizeof(out->member_uuid), row[1])
        || db_copy_text(out->login_id, sizeof(out->login_id), row[2])
        || db_copy_text(out->password_hash, sizeof(out->password_hash), row[3])
        || db_copy_text(out->created_at, sizeof(out->created_at), row[4]);
    out->member_id = (uint64_t)strtoull(row[0], NULL, 10);
    mysql_free_result(result);
    if (bad) { memset(out, 0, sizeof(*out)); return db_set_error(db, DB_DATABASE_ERROR, "Invalid Member data"); }
    return DB_OK;
}

DbResult find_member_by_uuid(Db *db, const char *uuid, AccountInfo *out)
{
    char normalized[37];
    if (!out || !valid_uuid(uuid, normalized)) return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid member UUID/output");
    memset(out, 0, sizeof(*out));
    DbResult status = db_query(db, "SELECT member_id,member_uuid,login_id,password_hash,created_at "
                               "FROM Member WHERE member_uuid='%s'", normalized);
    return status == DB_OK ? read_account(db, out) : status;
}

DbResult find_member_by_login_id(Db *db, const char *login_id, AccountInfo *out)
{
    if (!out || !valid_login_id(login_id)) return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid login ID/output");
    memset(out, 0, sizeof(*out));
    char *login = db_text_value(login_id);
    if (!login) return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate login ID");
    DbResult status = db_query(db, "SELECT member_id,member_uuid,login_id,password_hash,created_at "
                               "FROM Member WHERE login_id=%s", login);
    free(login);
    return status == DB_OK ? read_account(db, out) : status;
}

DbResult insert_member(Db *db, const char *uuid, const char *login_id,
                       const char *password_hash, uint64_t *member_id)
{
    if (member_id) *member_id = 0;
    char normalized[37];
    if (!member_id || !valid_uuid(uuid, normalized) || !valid_login_id(login_id)
            || !password_hash || !*password_hash || strlen(password_hash) > 255)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Member UUID, login ID, password hash and output required");
    char *login = db_text_value(login_id), *hash = db_text_value(password_hash);
    if (!login || !hash) {
        free(login); free(hash); return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate member values");
    }
    DbResult status = db_query(db, "INSERT INTO Member(member_uuid,login_id,password_hash) "
                               "VALUES('%s',%s,%s)", normalized, login, hash);
    free(login); free(hash);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        return db_set_error(db, DB_ALREADY_EXISTS, "Member UUID or login ID already registered");
    if (status == DB_OK) *member_id = (uint64_t)mysql_insert_id(db->connection);
    return status;
}

