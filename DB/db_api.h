#ifndef SPEAKER_DB_API_H
#define SPEAKER_DB_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct Db Db;

typedef enum {
    DB_OK = 0,
    DB_NOT_FOUND,
    DB_ALREADY_EXISTS,
    DB_ALREADY_PLAN,
    DB_INVALID_ARGUMENT,
    DB_BUSY,
    DB_RUNTIME_UNAVAILABLE,
    DB_CONFLICT,
    DB_FILE_ERROR,
    DB_DATABASE_ERROR,
    DB_MEMORY_ERROR,
    DB_RECOVERY_REQUIRED,
    DB_CLEANUP_PENDING
} DbResult;

typedef struct {
    const char *host, *user, *password, *database;
    const char *audio_directory;
    unsigned int port;
    /* Set only for management while the streaming server is stopped. */
    int offline;
} DbConfig;

typedef struct {
    long long song_id, duration, file_size_bytes;
    char title[801], file_path[2001], codec[65], checksum_sha256[65];
    int sample_rate_hz, channel_count;
} SongInfo;

typedef struct {
    uint64_t device_id;
    uint64_t member_id; /* 0 means no linked member; DB stores NULL. */
    char status[65];
    int plan_id;
} DeviceInfo;

typedef struct {
    int plan_id, bitrate_bps, sample_rate_hz, channel_count;
    char plan_code[81], plan_name[201], codec[65];
} PlanInfo;

typedef struct {
    PlanInfo plan;
    unsigned long long member_count, active_count;
} PlanCount;

typedef struct {
    int connected;
    char playback_state[32];
    long long current_song_id;
    unsigned long long last_played_pts_ms;
} MemberRuntime;

typedef struct {
    DeviceInfo device;
    PlanInfo plan;
    int runtime_available;
    MemberRuntime runtime;
} MemberInfo;

/* The server implements these callbacks with its session state/locks.
 * get_member: 0 = snapshot available (including disconnected), -1 = unavailable.
 * reserve_song_delete: 0 = reserved, 1 = in use, -1 = unavailable.
 * A reservation prevents the audio thread from opening the song until release.
 */
typedef struct {
    void *context;
    int (*get_member)(void *, uint64_t device_id, MemberRuntime *out);
    int (*reserve_song_delete)(void *, long long song_id);
    void (*release_song_delete)(void *, long long song_id);
} DbRuntimeHooks;

DbResult db_open(Db **out, const DbConfig *config, const DbRuntimeHooks *hooks);
void db_close(Db *db);
const char *db_error(const Db *db);
const char *db_result_name(DbResult result);

DbResult insert_device(Db *db, uint64_t device_id, int plan_id);
DbResult delete_device(Db *db, uint64_t device_id);
DbResult update_plan(Db *db, uint64_t device_id, int plan_id);
DbResult select_member(Db *db, uint64_t device_id, MemberInfo *out);


/* AccountInfo is the login account, unlike the existing device MemberInfo.
 * password_hash is server-only: never include it in an HTTP response or logs.
 */
typedef struct {
    uint64_t member_id;
    char member_uuid[37], login_id[401], password_hash[256], created_at[20];
} AccountInfo;

/* The server generates the UUID and hashes the password BEFORE calling this.
 * This function stores a hash; it does not implement password verification.
 */
DbResult insert_member(Db *db, const char *uuid, const char *login_id,
                       const char *password_hash, uint64_t *member_id);
DbResult find_member_by_uuid(Db *db, const char *uuid, AccountInfo *out);
DbResult find_member_by_login_id(Db *db, const char *login_id, AccountInfo *out);
/* NOT_FOUND = no device, CONFLICT = more than one linked device. */
DbResult find_device_by_member(Db *db, uint64_t member_id, DeviceInfo *out);
/* Call only after server authentication. Serialized on the Member row to
 * prevent two concurrent logins from creating two devices through this API.
 * ALREADY_EXISTS = member already has a device; fetch and reuse its ID.
 * The server supplies the generated device ID; a duplicate ID must be retried.
 * Do not call inside another transaction, or share a Db across threads.
 */
DbResult insert_member_device(Db *db, uint64_t member_id,
                              uint64_t device_id, int plan_id);

/* Caller owns returned list arrays and releases them with free(). */
DbResult list_song(Db *db, SongInfo **out, size_t *count);
DbResult list_device(Db *db, DeviceInfo **out, size_t *count);
DbResult list_plan(Db *db, PlanCount **out, size_t *count,
                   unsigned long long *total_members);

/* MP3 stays on disk; metadata is registered. Existing title is preserved.
 * Exact SHA-256 match at a different path is reported as ALREADY_EXISTS.
 */
DbResult insert_song(Db *db, const char *file_path, long long *song_id);
/* Deletes both DB row and physical MP3, with staging/recovery handling. */
DbResult delete_song(Db *db, long long song_id);

#endif
