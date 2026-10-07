#include "db_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_read_device(MYSQL_ROW row, DeviceInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->member_id = row[1] ? (uint64_t)strtoull(row[1], NULL, 10) : 0;
    return db_copy_text(out->device_uuid, sizeof(out->device_uuid), row[0])
        || db_copy_text(out->plan_name, sizeof(out->plan_name), row[2])
        || db_copy_text(out->status, sizeof(out->status), row[3]);
}

DbResult insert_device(Db *db, const char *uuid, const char *plan_name)
{
    if (!db || !uuid || strlen(uuid) != 36 || !plan_name || !*plan_name || strlen(plan_name) > 200)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Device UUID and plan name required");
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid UUID");
        } else if (!isxdigit((unsigned char)uuid[i]))
            return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid UUID");
    }
    char *u = db_text_value(uuid), *p = db_text_value(plan_name);
    if (!u || !p) { free(u); free(p); return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate values"); }
    DbResult status = db_query(db,
        "INSERT INTO Device(device_uuid,plan_name,status) VALUES(%s,%s,'ACTIVE')", u, p);
    free(u); free(p);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        return db_set_error(db, DB_ALREADY_EXISTS, "Device UUID already registered");
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1452)
        return db_set_error(db, DB_NOT_FOUND, "Plan not found");
    return status;
}

DbResult delete_device(Db *db, const char *uuid)
{
    if (!db || !uuid || strlen(uuid) != 36) return db_set_error(db, DB_INVALID_ARGUMENT, "UUID required");
    char *value = db_text_value(uuid);
    if (!value) return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate UUID");
    DbResult status = db_query(db, "DELETE FROM Device WHERE device_uuid=%s", value);
    free(value);
    if (status != DB_OK) return status;
    return mysql_affected_rows(db->connection) ? DB_OK : db_set_error(db, DB_NOT_FOUND, "Device not found");
}

DbResult update_plan(Db *db, const char *uuid, const char *plan_name)
{
    if (!db || !uuid || strlen(uuid) != 36 || !plan_name || !*plan_name || strlen(plan_name) > 200)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Plan name required");
    char *plan = db_text_value(plan_name), *value = db_text_value(uuid);
    if (!plan || !value) { free(plan); free(value); return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate values"); }
    /* Check existence and duplicate request together. No server state/locks. */
    DbResult status = db_query(db,
        "SELECT d.plan_name,p.plan_name,(d.plan_name=p.plan_name) FROM Device d LEFT JOIN Plan p ON p.plan_name=%s "
        "WHERE d.device_uuid=%s", plan, value);
    MYSQL_RES *result = NULL;
    if (status == DB_OK) status = db_get_result(db, &result);
    if (status == DB_OK) {
        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row) status = db_set_error(db, DB_NOT_FOUND, "Device not found");
        else if (!row[1]) status = db_set_error(db, DB_NOT_FOUND, "Plan not found");
        else if (row[2] && !strcmp(row[2], "1")) status = db_set_error(db, DB_ALREADY_PLAN, "Already subscribed to %s", row[1]);
        mysql_free_result(result);
    }
    if (status == DB_OK)
        status = db_query(db, "UPDATE Device SET plan_name=%s WHERE device_uuid=%s", plan, value);
    free(plan); free(value);
    return status;
}

DbResult list_device(Db *db, DeviceInfo **out, size_t *count)
{
    if (!db || !out || !count) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = db_query(db,
        "SELECT device_uuid,member_id,plan_name,status FROM Device ORDER BY device_uuid");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (db_read_device(row, &(*out)[*count])) {
                status = db_set_error(db, DB_DATABASE_ERROR, "Invalid Device data"); break;
            }
            ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; }
    return status;
}

DbResult list_plan(Db *db, PlanCount **out, size_t *count, unsigned long long *total)
{
    if (!db || !out || !count || !total) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0; *total = 0;
    DbResult status = db_query(db,
        "SELECT p.plan_name,COUNT(DISTINCT d.member_id) FROM Plan p "
        "LEFT JOIN Device d ON d.plan_name=p.plan_name GROUP BY p.plan_name ORDER BY p.plan_name");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (db_copy_text((*out)[*count].plan_name, sizeof((*out)[*count].plan_name), row[0])) {
                status = db_set_error(db, DB_DATABASE_ERROR, "Invalid Plan data"); break;
            }
            (*out)[*count].member_count = strtoull(row[1], NULL, 10);
            ++*count;
        }
    }
    mysql_free_result(result);
    if (status == DB_OK) {
        status = db_query(db, "SELECT COUNT(DISTINCT member_id) FROM Device");
        if (status == DB_OK) status = db_get_result(db, &result);
        if (status == DB_OK) {
            MYSQL_ROW row = mysql_fetch_row(result);
            if (row) *total = strtoull(row[0], NULL, 10);
            else status = db_set_error(db, DB_DATABASE_ERROR, "Cannot count members");
            mysql_free_result(result);
        }
    }
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; *total = 0; }
    return status;
}
