#include "db_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Here "member" means the agreed device/member information query.
 * Account signup, password verification and sessions are not implemented.
 */
DbResult select_member(Db *db, const char *uuid, MemberInfo *out)
{
    if (!db || !out || !uuid || strlen(uuid) >= 36)
    {
        puts("1 error\n");
        return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    }
    memset(out, 0, sizeof(*out));
    char *value = db_text_value(uuid);
    if (!value)
    {
        puts("no value\n");

        return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate UUID");
    }
    DbResult status = db_query(db,
                               "SELECT d.device_uuid,d.member_id,d.plan_name,d.status,p.codec,p.bitrate_bps "
                               "FROM Device d JOIN Plan p ON p.plan_name=d.plan_name WHERE d.device_uuid=%s",
                               value);
    free(value);
    if (status != DB_OK)
    {
        puts("query status not ok\n");
        return status;
    }
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK)
    {
        puts("get db status not ok\n");
        return status;
    }
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row)
    {
        puts("no row\n");
        mysql_free_result(result);
        return db_set_error(db, DB_NOT_FOUND, "Device/plan not found");
    }
    int bad = db_read_device(row, &out->device) || db_copy_text(out->plan.plan_name, sizeof(out->plan.plan_name), row[2]) || db_copy_text(out->plan.codec, sizeof(out->plan.codec), row[4]);
    out->plan.bitrate_bps = atoi(row[5]);
    mysql_free_result(result);
    if (bad)
    {
        puts("bad\n");
        return db_set_error(db, DB_DATABASE_ERROR, "Invalid member data");
    }
    /* These values are owned by the running streaming server, not MariaDB. */
    if (db->runtime.get_member)
    {
        MemberRuntime snapshot = {0};
        if (db->runtime.get_member(db->runtime.context, uuid, &snapshot) == 0)
        {
            out->runtime = snapshot;
            out->runtime_available = 1;
        }
    }
    return DB_OK;
}
