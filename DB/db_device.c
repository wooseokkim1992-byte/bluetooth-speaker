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

static int parse_device(MYSQL_ROW row, DeviceInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->device_id = (uint64_t)strtoull(row[0], NULL, 10);
    out->plan_id = atoi(row[1]);
    out->member_id = row[3] ? (uint64_t)strtoull(row[3], NULL, 10) : 0;
    return db_copy_text(out->status, sizeof(out->status), row[2]);
}

static DbResult get_device(Db *db, uint64_t device_id, DeviceInfo *out)
{
    DbResult status = db_query(db, "SELECT device_id,plan_id,status,member_id FROM Device "
                               "WHERE device_id=%llu", (unsigned long long)device_id);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return db_set_error(db, DB_NOT_FOUND, "Device not found"); }
    int bad = parse_device(row, out);
    mysql_free_result(result);
    return bad ? db_set_error(db, DB_DATABASE_ERROR, "Invalid Device data") : DB_OK;
}

static int parse_plan(MYSQL_ROW row, PlanInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->plan_id = atoi(row[0]);
    out->bitrate_bps = atoi(row[4]);
    out->sample_rate_hz = atoi(row[5]);
    out->channel_count = atoi(row[6]);
    return db_copy_text(out->plan_code, sizeof(out->plan_code), row[1])
        || db_copy_text(out->plan_name, sizeof(out->plan_name), row[2])
        || db_copy_text(out->codec, sizeof(out->codec), row[3]);
}

static DbResult get_plan(Db *db, int id, const char *code, PlanInfo *out)
{
    DbResult status;
    const char *columns = "plan_id,plan_code,plan_name,codec,bitrate_bps,sample_rate_hz,channel_count";
    if (code) status = db_query(db, "SELECT %s FROM Plan WHERE plan_code='%s'", columns, code);
    else status = db_query(db, "SELECT %s FROM Plan WHERE plan_id=%d", columns, id);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return db_set_error(db, DB_NOT_FOUND, "Plan not found"); }
    int bad = parse_plan(row, out);
    mysql_free_result(result);
    return bad ? db_set_error(db, DB_DATABASE_ERROR, "Invalid Plan data") : DB_OK;
}

DbResult insert_device(Db *db, uint64_t device_id, int plan_id)
{
    if (plan_id <= 0) return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid plan ID");
    PlanInfo plan;
    DbResult status = get_plan(db, plan_id, NULL, &plan);
    if (status != DB_OK) return status;
    status = db_query(db, "INSERT INTO Device(device_id,auth_token_hash,plan_id,status) "
                      "VALUES(%llu,NULL,%d,'ACTIVE')", (unsigned long long)device_id, plan_id);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        return db_set_error(db, DB_ALREADY_EXISTS, "Device ID already registered");
    return status;
}

DbResult delete_device(Db *db, uint64_t device_id)
{
    if (!db->offline) {
        MemberRuntime runtime = {0};
        if (!db->runtime.get_member || db->runtime.get_member(db->runtime.context, device_id, &runtime))
            return db_set_error(db, DB_RUNTIME_UNAVAILABLE, "Server connection state is required");
        if (runtime.connected) return db_set_error(db, DB_BUSY, "Disconnect the device first");
    }
    DbResult status = db_query(db, "DELETE FROM Device WHERE device_id=%llu", (unsigned long long)device_id);
    if (status != DB_OK) return status;
    return mysql_affected_rows(db->connection) ? DB_OK
        : db_set_error(db, DB_NOT_FOUND, "Device not found");
}

DbResult update_plan(Db *db, uint64_t device_id, int plan_id)
{
    if (plan_id <= 0) return db_set_error(db, DB_INVALID_ARGUMENT, "Invalid plan ID");
    DeviceInfo device;
    PlanInfo target;
    DbResult status = get_device(db, device_id, &device);
    if (status != DB_OK) return status;
    status = get_plan(db, plan_id, NULL, &target);
    if (status != DB_OK) return status;
    if (device.plan_id == target.plan_id)
        return db_set_error(db, DB_ALREADY_PLAN, "Already subscribed to %s", target.plan_code);
    status = db_query(db, "UPDATE Device SET plan_id=%d WHERE device_id=%llu AND plan_id=%d",
                   target.plan_id, (unsigned long long)device_id, device.plan_id);
    if (status != DB_OK) return status;
    return mysql_affected_rows(db->connection) ? DB_OK
        : db_set_error(db, DB_CONFLICT, "Device changed during plan update; read it again");
}

DbResult select_member(Db *db, uint64_t device_id, MemberInfo *out)
{
    if (!out) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    memset(out, 0, sizeof(*out));
    DbResult status = get_device(db, device_id, &out->device);
    if (status != DB_OK) return status;
    if ((status = get_plan(db, out->device.plan_id, NULL, &out->plan)) != DB_OK) return status;
    if (db->offline) {
        out->runtime_available = 1;
        strcpy(out->runtime.playback_state, "DISCONNECTED");
    } else if (db->runtime.get_member && !db->runtime.get_member(db->runtime.context, device_id, &out->runtime))
        out->runtime_available = 1;
    return DB_OK;
}

DbResult find_device_by_member(Db *db, uint64_t member_id, DeviceInfo *out)
{
    if (!member_id || !out) return db_set_error(db, DB_INVALID_ARGUMENT, "Member ID/output required");
    memset(out, 0, sizeof(*out));
    DbResult status = db_query(db, "SELECT device_id,plan_id,status,member_id FROM Device "
                               "WHERE member_id=%llu ORDER BY device_id LIMIT 2", (unsigned long long)member_id);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    if (mysql_num_rows(result) > 1) {
        mysql_free_result(result); return db_set_error(db, DB_CONFLICT, "Member has multiple devices");
    }
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return db_set_error(db, DB_NOT_FOUND, "Member has no device"); }
    int bad = parse_device(row, out);
    mysql_free_result(result);
    return bad ? db_set_error(db, DB_DATABASE_ERROR, "Invalid Device data") : DB_OK;
}

static DbResult rollback_member_device(Db *db, DbResult status)
{
    /* Do not clear the original diagnostic via db_query(). */
    if (mysql_query(db->connection, "ROLLBACK"))
        return db_set_error(db, DB_RECOVERY_REQUIRED, "Rollback failed; close DB connection");
    return status;
}

DbResult insert_member_device(Db *db, uint64_t member_id, uint64_t device_id, int plan_id)
{
    if (!member_id || plan_id <= 0) return db_set_error(db, DB_INVALID_ARGUMENT, "Member ID and plan ID required");
    PlanInfo plan;
    DbResult status = get_plan(db, plan_id, NULL, &plan);
    if (status != DB_OK) return status;
    if ((status = db_query(db, "START TRANSACTION")) != DB_OK) return status;
    status = db_query(db, "SELECT member_id FROM Member WHERE member_id=%llu FOR UPDATE",
                   (unsigned long long)member_id);
    if (status != DB_OK) return rollback_member_device(db, status);
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return rollback_member_device(db, status);
    int exists = mysql_num_rows(result) != 0;
    mysql_free_result(result);
    if (!exists) return rollback_member_device(db, db_set_error(db, DB_NOT_FOUND, "Member not found"));
    DeviceInfo device;
    status = find_device_by_member(db, member_id, &device);
    if (status == DB_OK)
        return rollback_member_device(db, db_set_error(db, DB_ALREADY_EXISTS, "Member already has a device; reuse it"));
    if (status != DB_NOT_FOUND) return rollback_member_device(db, status);
    status = db_query(db, "INSERT INTO Device(device_id,member_id,auth_token_hash,plan_id,status) "
                      "VALUES(%llu,%llu,NULL,%d,'ACTIVE')",
                   (unsigned long long)device_id, (unsigned long long)member_id, plan_id);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        status = db_set_error(db, DB_CONFLICT, "Device ID already exists; generate a new ID and retry");
    if (status != DB_OK) return rollback_member_device(db, status);
    status = db_query(db, "COMMIT");
    if (status != DB_OK) return db_set_error(db, DB_RECOVERY_REQUIRED,
        "Commit outcome unknown; reconnect and query member device before retrying");
    return DB_OK;
}

DbResult list_device(Db *db, DeviceInfo **out, size_t *count)
{
    if (!out || !count) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = db_query(db, "SELECT device_id,plan_id,status,member_id FROM Device ORDER BY device_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            DeviceInfo *device = &(*out)[*count];
            if (parse_device(row, device)) {
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
    if (!out || !count || !total) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0; *total = 0;
    DbResult status = db_query(db,
        "SELECT p.plan_id,p.plan_code,p.plan_name,p.codec,p.bitrate_bps,p.sample_rate_hz,p.channel_count,"
        "COUNT(d.device_id),COALESCE(SUM(d.status='ACTIVE'),0) FROM Plan p "
        "LEFT JOIN Device d ON d.plan_id=p.plan_id "
        "GROUP BY p.plan_id,p.plan_code,p.plan_name,p.codec,p.bitrate_bps,p.sample_rate_hz,p.channel_count "
        "ORDER BY p.plan_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            PlanCount *plan = &(*out)[*count];
            if (parse_plan(row, &plan->plan)) { status = db_set_error(db, DB_DATABASE_ERROR, "Invalid Plan data"); break; }
            plan->member_count = strtoull(row[7], NULL, 10);
            plan->active_count = strtoull(row[8], NULL, 10);
            *total += plan->member_count; ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; *total = 0; }
    return status;
}

