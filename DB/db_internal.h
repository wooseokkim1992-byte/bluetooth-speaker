#ifndef SPEAKER_DB_INTERNAL_H
#define SPEAKER_DB_INTERNAL_H

/* Only DB implementation files include this header. */
#include "db_api.h"
#include <mysql.h>
#include <limits.h>

struct Db {
    MYSQL *connection;
    char audio_directory[PATH_MAX], error[1024];
    int offline;
    DbRuntimeHooks runtime;
};

DbResult db_set_error(Db *db, DbResult result, const char *format, ...);
DbResult db_query(Db *db, const char *format, ...);
DbResult db_get_result(Db *db, MYSQL_RES **out);
int db_copy_text(char *out, size_t capacity, const char *value);
char *db_text_value(const char *text);
DbResult db_allocate_list(Db *db, MYSQL_RES *result, size_t element_size,
                          void **out, size_t *count);

#endif
