#ifndef SPEAKER_DB_API_H
#define SPEAKER_DB_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct Db Db;
typedef enum {
    DB_OK = 0, DB_NOT_FOUND, DB_ALREADY_EXISTS, DB_ALREADY_PLAN,
    DB_INVALID_ARGUMENT, DB_DATABASE_ERROR, DB_MEMORY_ERROR, DB_FILE_ERROR
} DbResult;

typedef struct {
    const char *host, *user, *password, *database;
    unsigned int port;
} DbConfig;

typedef struct {
    uint64_t member_id; /* member_id=0 means SQL NULL. */
    char device_uuid[37], plan_name[201], status[65];
} DeviceInfo;

typedef struct {
    char plan_name[201], codec[65];
    int bitrate_bps;
} PlanInfo;

typedef struct {
    char plan_name[201];
    unsigned long long member_count;
} PlanCount;

typedef struct {
    long long song_id, duration, file_size_bytes; /* duration: seconds */
    char title[801], file_path[2001], codec[65], checksum_sha256[65];
} SongInfo;

typedef struct {
    int connected;
    char playback_state[32];
    long long current_song_id;
    unsigned long long last_played_pts_ms; /* protocol PTS: milliseconds */
} MemberRuntime;

typedef struct {
    DeviceInfo device;
    PlanInfo plan;
    int runtime_available;
    MemberRuntime runtime;
} MemberInfo;

/* Optional server-provided snapshot for select_member().
 * 0 = snapshot available, -1 = unavailable. No deletion/locking callbacks.
 * Without this callback only DB information is returned.
 */
typedef struct {
    void *context;
    int (*get_member)(void *context, const char *device_uuid, MemberRuntime *out);
} DbRuntimeHooks;

/* Connection/error helpers, not additional business APIs.
 * Each Db connection is used by one caller at a time.
 */
DbResult db_open(Db **out, const DbConfig *config, const DbRuntimeHooks *hooks);
void db_close(Db *db);
const char *db_error(const Db *db);
const char *db_result_name(DbResult result);

/* The nine agreed business APIs. UUID is supplied during device insertion.
 * Member linking/login and token enrollment are outside this package.
 */
DbResult insert_device(Db *db, const char *device_uuid, const char *plan_name);
DbResult delete_device(Db *db, const char *device_uuid);
DbResult update_plan(Db *db, const char *device_uuid, const char *plan_name);
DbResult select_member(Db *db, const char *device_uuid, MemberInfo *out);
DbResult list_song(Db *db, SongInfo **out, size_t *count);
DbResult list_device(Db *db, DeviceInfo **out, size_t *count);
DbResult list_plan(Db *db, PlanCount **out, size_t *count,
                   unsigned long long *total_members);
DbResult insert_song(Db *db, const char *file_path, long long *song_id);
DbResult delete_song(Db *db, long long song_id);

/* list_* arrays belong to the caller; release them with free().
 * The server must handle disconnection/file-use checks BEFORE calling delete.
 * delete_song deletes both the MP3 and DB row; it has no automatic recovery.
 */
#endif
