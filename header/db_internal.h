#ifndef SPEAKER_DB_INTERNAL_H
#define SPEAKER_DB_INTERNAL_H

#include "db_api.h"
#include <mysql.h>
#define PLAN_BASE "BASE"
#define PLAN_PREMIUM "PREMIUM"
struct Db
{
    MYSQL *connection;
    char error[512];
    DbRuntimeHooks runtime;
};
DbResult db_set_error(Db *db, DbResult result, const char *format, ...);
DbResult db_query(Db *db, const char *format, ...);
DbResult db_get_result(Db *db, MYSQL_RES **out);
char *db_text_value(const char *text);
int db_copy_text(char *out, size_t capacity, const char *value);
DbResult db_allocate_list(Db *db, MYSQL_RES *result, size_t item_size, void **out);
/* Same SELECT order in list_device and select_member. */
int db_read_device(MYSQL_ROW row, DeviceInfo *out);
#endif
