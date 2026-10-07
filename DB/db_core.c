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

DbResult db_set_error(Db *db, DbResult result, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(db->error, sizeof(db->error), format, args);
    va_end(args);
    return result;
}

const char *db_error(const Db *db) { return db ? db->error : "DB is not open"; }
const char *db_result_name(DbResult result)
{
    static const char *names[] = {
        "OK", "NOT_FOUND", "ALREADY_EXISTS", "ALREADY_PLAN", "INVALID_ARGUMENT",
        "BUSY", "RUNTIME_UNAVAILABLE", "CONFLICT", "FILE_ERROR", "DATABASE_ERROR",
        "MEMORY_ERROR", "RECOVERY_REQUIRED", "CLEANUP_PENDING"
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "UNKNOWN";
}

/* Values are SQL UTF-8 hex expressions, never interpolated as SQL syntax. */
char *db_text_value(const char *text)
{
    size_t length = strlen(text);
    if (length > (SIZE_MAX - 40) / 2) return NULL;
    char *value = malloc(2 * length + 40);
    if (!value) return NULL;
    char *p = value;
    p += sprintf(p, "CONVERT(0x");
    for (size_t i = 0; i < length; ++i) p += sprintf(p, "%02x", (unsigned char)text[i]);
    strcpy(p, " USING utf8mb4)");
    return value;
}

DbResult db_query(Db *db, const char *format, ...)
{
    db->error[0] = '\0';
    va_list args;
    va_start(args, format);
    char *sql = NULL;
    int length = vasprintf(&sql, format, args);
    va_end(args);
    if (length < 0) return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate SQL");
    int result = mysql_real_query(db->connection, sql, (unsigned long)length);
    free(sql);
    if (result) return db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

DbResult db_get_result(Db *db, MYSQL_RES **out)
{
    *out = mysql_store_result(db->connection);
    if (!*out) return db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

int db_copy_text(char *out, size_t capacity, const char *value)
{
    if (!value || strlen(value) >= capacity) return -1;
    strcpy(out, value);
    return 0;
}

DbResult db_open(Db **out, const DbConfig *config, const DbRuntimeHooks *hooks)
{
    if (!out || !config || !config->audio_directory) return DB_INVALID_ARGUMENT;
    *out = NULL;
    Db *db = calloc(1, sizeof(*db));
    if (!db) return DB_MEMORY_ERROR;
    /* Keep the context on failure so the caller can inspect db_error/db_close. */
    *out = db;
    if (!realpath(config->audio_directory, db->audio_directory))
        return db_set_error(db, DB_FILE_ERROR, "Audio directory: %s", strerror(errno));
    struct stat st;
    if (stat(db->audio_directory, &st) || !S_ISDIR(st.st_mode))
        return db_set_error(db, DB_FILE_ERROR, "Audio path is not a directory");
    db->offline = config->offline;
    if (hooks) db->runtime = *hooks;
    db->connection = mysql_init(NULL);
    if (!db->connection) return db_set_error(db, DB_MEMORY_ERROR, "mysql_init failed");
    unsigned int timeout = 5;
    mysql_options(db->connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (!mysql_real_connect(db->connection, config->host, config->user, config->password,
            config->database, config->port, NULL, 0))
        return db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    if (mysql_set_character_set(db->connection, "utf8mb4"))
        return db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

void db_close(Db *db)
{
    if (!db) return;
    if (db->connection) mysql_close(db->connection);
    free(db);
}

DbResult db_allocate_list(Db *db, MYSQL_RES *result, size_t element_size,
                              void **out, size_t *count)
{
    my_ulonglong rows = mysql_num_rows(result);
    *count = 0; *out = NULL;
    if (rows > SIZE_MAX / element_size) return db_set_error(db, DB_MEMORY_ERROR, "List is too large");
    if (rows) {
        *out = calloc((size_t)rows, element_size);
        if (!*out) return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate list");
    }
    return DB_OK;
}

