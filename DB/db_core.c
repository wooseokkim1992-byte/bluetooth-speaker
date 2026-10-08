#define _GNU_SOURCE
#include "db_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DbResult db_set_error(Db *db, DbResult result, const char *format, ...)
{
    if (db)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(db->error, sizeof(db->error), format, args);
        va_end(args);
    }
    return result;
}

const char *db_error(const Db *db)
{
    return db ? db->error : "DB is not open";
}

const char *db_result_name(DbResult result)
{
    static const char *names[] = {
        "OK", "NOT_FOUND", "ALREADY_EXISTS", "ALREADY_PLAN",
        "INVALID_ARGUMENT", "DATABASE_ERROR", "MEMORY_ERROR", "FILE_ERROR"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "UNKNOWN";
}

DbResult db_open(Db **out, const DbConfig *config, const DbRuntimeHooks *hooks)
{
    if (!out || !config)
        return DB_INVALID_ARGUMENT;
    *out = calloc(1, sizeof(Db));
    if (!*out)
        return DB_MEMORY_ERROR;
    Db *db = *out; /* Caller can inspect db_error() even on connection failure. */
    if (hooks)
        db->runtime = *hooks;
    db->connection = mysql_init(NULL);
    if (!db->connection)
        return db_set_error(db, DB_MEMORY_ERROR, "mysql_init failed");
    if (!mysql_real_connect(db->connection, config->host, config->user,
                            config->password, config->database, config->port, NULL, 0) ||
        mysql_set_character_set(db->connection, "utf8mb4"))
    {
        printf("db connection err\n");
        return db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    }
    return DB_OK;
}

void db_close(Db *db)
{
    if (!db)
        return;
    if (db->connection)
        mysql_close(db->connection);
    free(db);
}

DbResult db_query(Db *db, const char *format, ...)
{
    if (!db || !db->connection)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Open DB first");
    db->error[0] = '\0';
    va_list args;
    va_start(args, format);
    char *sql = NULL;
    int length = vasprintf(&sql, format, args);
    va_end(args);
    if (length < 0)
        return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate SQL");
    fprintf(stdout, "sql:%s\n", sql);
    int failed = mysql_real_query(db->connection, sql, (unsigned long)length);
    free(sql);
    return failed ? db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection)) : DB_OK;
}

DbResult db_get_result(Db *db, MYSQL_RES **out)
{
    *out = mysql_store_result(db->connection);
    return *out ? DB_OK : db_set_error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
}

/* A UTF-8 SQL hex expression preserves quotes in titles/paths and matches
 * the existing DB collation. Text is never inserted as raw SQL syntax.
 */
char *db_text_value(const char *text)
{
    size_t length = strlen(text);
    if (length > (SIZE_MAX - 80) / 2)
        return NULL;
    char *value = malloc(length * 2 + 80);
    if (!value)
        return NULL;
    char *p = value;
    p += sprintf(p, "CONVERT(0x");
    for (size_t i = 0; i < length; ++i)
        p += sprintf(p, "%02x", (unsigned char)text[i]);
    strcpy(p, " USING utf8mb4) COLLATE utf8mb4_unicode_ci");
    return value;
}

int db_copy_text(char *out, size_t capacity, const char *value)
{
    if (!value || strlen(value) >= capacity)
        return -1;
    strcpy(out, value);
    return 0;
}

DbResult db_allocate_list(Db *db, MYSQL_RES *result, size_t item_size, void **out)
{
    my_ulonglong rows = mysql_num_rows(result);
    *out = NULL;
    if (rows > SIZE_MAX / item_size)
        return db_set_error(db, DB_MEMORY_ERROR, "List too large");
    if (rows && !(*out = calloc((size_t)rows, item_size)))
        return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate list");
    return DB_OK;
}
