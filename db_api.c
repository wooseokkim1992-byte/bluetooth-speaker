#define _GNU_SOURCE
#include "db_api.h"
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

struct Db {
    MYSQL *connection;
    char audio_directory[PATH_MAX], error[1024];
    int offline;
    DbRuntimeHooks runtime;
};

static DbResult error(Db *db, DbResult result, const char *format, ...)
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
static char *text_value(const char *text)
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

static DbResult query(Db *db, const char *format, ...)
{
    db->error[0] = '\0';
    va_list args;
    va_start(args, format);
    char *sql = NULL;
    int length = vasprintf(&sql, format, args);
    va_end(args);
    if (length < 0) return error(db, DB_MEMORY_ERROR, "Cannot allocate SQL");
    int result = mysql_real_query(db->connection, sql, (unsigned long)length);
    free(sql);
    if (result) return error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

static DbResult get_result(Db *db, MYSQL_RES **out)
{
    *out = mysql_store_result(db->connection);
    if (!*out) return error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

static int copy_text(char *out, size_t capacity, const char *value)
{
    if (!value || strlen(value) >= capacity) return -1;
    strcpy(out, value);
    return 0;
}

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

DbResult db_open(Db **out, const DbConfig *config, const DbRuntimeHooks *hooks)
{
    if (!out || !config || !config->audio_directory) return DB_INVALID_ARGUMENT;
    *out = NULL;
    Db *db = calloc(1, sizeof(*db));
    if (!db) return DB_MEMORY_ERROR;
    /* Keep the context on failure so the caller can inspect db_error/db_close. */
    *out = db;
    if (!realpath(config->audio_directory, db->audio_directory))
        return error(db, DB_FILE_ERROR, "Audio directory: %s", strerror(errno));
    struct stat st;
    if (stat(db->audio_directory, &st) || !S_ISDIR(st.st_mode))
        return error(db, DB_FILE_ERROR, "Audio path is not a directory");
    db->offline = config->offline;
    if (hooks) db->runtime = *hooks;
    db->connection = mysql_init(NULL);
    if (!db->connection) return error(db, DB_MEMORY_ERROR, "mysql_init failed");
    unsigned int timeout = 5;
    mysql_options(db->connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (!mysql_real_connect(db->connection, config->host, config->user, config->password,
            config->database, config->port, NULL, 0))
        return error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    if (mysql_set_character_set(db->connection, "utf8mb4"))
        return error(db, DB_DATABASE_ERROR, "%s", mysql_error(db->connection));
    return DB_OK;
}

void db_close(Db *db)
{
    if (!db) return;
    if (db->connection) mysql_close(db->connection);
    free(db);
}

static DbResult get_device(Db *db, const char *uuid, DeviceInfo *out)
{
    DbResult status = query(db, "SELECT device_uuid,plan_id,status FROM Device "
                               "WHERE device_uuid='%s'", uuid);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return error(db, DB_NOT_FOUND, "Device not found"); }
    memset(out, 0, sizeof(*out));
    int bad = copy_text(out->uuid, sizeof(out->uuid), row[0])
            || copy_text(out->status, sizeof(out->status), row[2]);
    out->plan_id = atoi(row[1]);
    mysql_free_result(result);
    return bad ? error(db, DB_DATABASE_ERROR, "Invalid Device data") : DB_OK;
}

static int parse_plan(MYSQL_ROW row, PlanInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->plan_id = atoi(row[0]);
    out->bitrate_bps = atoi(row[4]);
    out->sample_rate_hz = atoi(row[5]);
    out->channel_count = atoi(row[6]);
    return copy_text(out->plan_code, sizeof(out->plan_code), row[1])
        || copy_text(out->plan_name, sizeof(out->plan_name), row[2])
        || copy_text(out->codec, sizeof(out->codec), row[3]);
}

static DbResult get_plan(Db *db, int id, const char *code, PlanInfo *out)
{
    DbResult status;
    const char *columns = "plan_id,plan_code,plan_name,codec,bitrate_bps,sample_rate_hz,channel_count";
    if (code) status = query(db, "SELECT %s FROM Plan WHERE plan_code='%s'", columns, code);
    else status = query(db, "SELECT %s FROM Plan WHERE plan_id=%d", columns, id);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return error(db, DB_NOT_FOUND, "Plan not found"); }
    int bad = parse_plan(row, out);
    mysql_free_result(result);
    return bad ? error(db, DB_DATABASE_ERROR, "Invalid Plan data") : DB_OK;
}

DbResult insert_device(Db *db, const char *uuid, int plan_id)
{
    char normalized[37];
    if (!valid_uuid(uuid, normalized) || plan_id <= 0)
        return error(db, DB_INVALID_ARGUMENT, "Use a 36-character UUID and valid plan ID");
    PlanInfo plan;
    DbResult status = get_plan(db, plan_id, NULL, &plan);
    if (status != DB_OK) return status;
    status = query(db, "INSERT INTO Device(device_uuid,auth_token_hash,plan_id,status) "
                      "VALUES('%s',NULL,%d,'ACTIVE')", normalized, plan_id);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        return error(db, DB_ALREADY_EXISTS, "Device already registered");
    return status;
}

DbResult delete_device(Db *db, const char *uuid)
{
    char normalized[37];
    if (!valid_uuid(uuid, normalized)) return error(db, DB_INVALID_ARGUMENT, "Invalid UUID");
    /* Refuse to silently leave an active connection after deleting registration. */
    if (!db->offline) {
        MemberRuntime runtime = {0};
        if (!db->runtime.get_member || db->runtime.get_member(db->runtime.context, normalized, &runtime))
            return error(db, DB_RUNTIME_UNAVAILABLE, "Server connection state is required");
        if (runtime.connected) return error(db, DB_BUSY, "Disconnect the device first");
    }
    DbResult status = query(db, "DELETE FROM Device WHERE device_uuid='%s'", normalized);
    if (status != DB_OK) return status;
    return mysql_affected_rows(db->connection) ? DB_OK
        : error(db, DB_NOT_FOUND, "Device not found");
}

static DbResult change_plan(Db *db, const char *uuid, const char *target_code,
                            const char *source_code)
{
    char normalized[37];
    if (!valid_uuid(uuid, normalized)) return error(db, DB_INVALID_ARGUMENT, "Invalid UUID");
    DeviceInfo device;
    PlanInfo current, target;
    DbResult status = get_device(db, normalized, &device);
    if (status != DB_OK) return status;
    if ((status = get_plan(db, device.plan_id, NULL, &current)) != DB_OK) return status;
    if (!strcmp(current.plan_code, target_code))
        return error(db, DB_ALREADY_PLAN, "Already subscribed to %s", target_code);
    if (strcmp(current.plan_code, source_code))
        return error(db, DB_INVALID_ARGUMENT, "Expected current plan %s", source_code);
    if ((status = get_plan(db, 0, target_code, &target)) != DB_OK) return status;
    status = query(db, "UPDATE Device SET plan_id=%d WHERE device_uuid='%s' AND plan_id=%d",
                   target.plan_id, normalized, device.plan_id);
    if (status != DB_OK) return status;
    return mysql_affected_rows(db->connection) ? DB_OK
        : error(db, DB_CONFLICT, "Device changed during plan update; read it again");
}

DbResult subscribe_plan(Db *db, const char *uuid) { return change_plan(db, uuid, "PREMIUM", "BASE"); }
DbResult cancel_plan(Db *db, const char *uuid) { return change_plan(db, uuid, "BASE", "PREMIUM"); }

DbResult select_member(Db *db, const char *uuid, MemberInfo *out)
{
    char normalized[37];
    if (!out || !valid_uuid(uuid, normalized)) return error(db, DB_INVALID_ARGUMENT, "Invalid UUID/output");
    memset(out, 0, sizeof(*out));
    DbResult status = get_device(db, normalized, &out->device);
    if (status != DB_OK) return status;
    if ((status = get_plan(db, out->device.plan_id, NULL, &out->plan)) != DB_OK) return status;
    if (db->offline) {
        out->runtime_available = 1;
        strcpy(out->runtime.playback_state, "DISCONNECTED");
    } else if (db->runtime.get_member && !db->runtime.get_member(db->runtime.context, normalized, &out->runtime))
        out->runtime_available = 1;
    return DB_OK;
}

static int parse_song(MYSQL_ROW row, SongInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->song_id = atoll(row[0]);
    out->sample_rate_hz = atoi(row[4]);
    out->channel_count = atoi(row[5]);
    out->duration = atoll(row[6]);
    out->file_size_bytes = atoll(row[7]);
    return copy_text(out->title, sizeof(out->title), row[1])
        || copy_text(out->file_path, sizeof(out->file_path), row[2])
        || copy_text(out->codec, sizeof(out->codec), row[3])
        || copy_text(out->checksum_sha256, sizeof(out->checksum_sha256), row[8]);
}

static DbResult allocate_list(Db *db, MYSQL_RES *result, size_t element_size,
                              void **out, size_t *count)
{
    my_ulonglong rows = mysql_num_rows(result);
    *count = 0; *out = NULL;
    if (rows > SIZE_MAX / element_size) return error(db, DB_MEMORY_ERROR, "List is too large");
    if (rows) {
        *out = calloc((size_t)rows, element_size);
        if (!*out) return error(db, DB_MEMORY_ERROR, "Cannot allocate list");
    }
    return DB_OK;
}

DbResult list_song(Db *db, SongInfo **out, size_t *count)
{
    if (!out || !count) return error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = query(db, "SELECT song_id,title,file_path,codec,sample_rate_hz,"
        "channel_count,duration,file_size_bytes,checksum_sha256 FROM Song ORDER BY song_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (parse_song(row, &(*out)[*count])) { status = error(db, DB_DATABASE_ERROR, "Invalid Song data"); break; }
            ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; }
    return status;
}

DbResult list_device(Db *db, DeviceInfo **out, size_t *count)
{
    if (!out || !count) return error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = query(db, "SELECT device_uuid,plan_id,status FROM Device ORDER BY device_uuid");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            DeviceInfo *device = &(*out)[*count];
            if (copy_text(device->uuid, sizeof(device->uuid), row[0])
                    || copy_text(device->status, sizeof(device->status), row[2])) {
                status = error(db, DB_DATABASE_ERROR, "Invalid Device data"); break;
            }
            device->plan_id = atoi(row[1]); ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; }
    return status;
}

DbResult list_plan(Db *db, PlanCount **out, size_t *count, unsigned long long *total)
{
    if (!out || !count || !total) return error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0; *total = 0;
    DbResult status = query(db,
        "SELECT p.plan_id,p.plan_code,p.plan_name,p.codec,p.bitrate_bps,p.sample_rate_hz,p.channel_count,"
        "COUNT(d.device_uuid),COALESCE(SUM(d.status='ACTIVE'),0) FROM Plan p "
        "LEFT JOIN Device d ON d.plan_id=p.plan_id "
        "GROUP BY p.plan_id,p.plan_code,p.plan_name,p.codec,p.bitrate_bps,p.sample_rate_hz,p.channel_count "
        "ORDER BY p.plan_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            PlanCount *plan = &(*out)[*count];
            if (parse_plan(row, &plan->plan)) { status = error(db, DB_DATABASE_ERROR, "Invalid Plan data"); break; }
            plan->member_count = strtoull(row[7], NULL, 10);
            plan->active_count = strtoull(row[8], NULL, 10);
            *total += plan->member_count; ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; *total = 0; }
    return status;
}

static int capture(char *const args[], char *out, size_t capacity)
{
    int pipes[2], status;
    if (pipe(pipes)) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (!child) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipes[1]); execvp(args[0], args); _exit(127);
    }
    close(pipes[1]);
    size_t used = 0;
    int bad = 0;
    char block[1024];
    ssize_t n;
    while ((n = read(pipes[0], block, sizeof(block))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; bad = 1; break; }
        if ((size_t)n > capacity - used - 1) bad = 1;
        else { memcpy(out + used, block, (size_t)n); used += (size_t)n; }
    }
    close(pipes[0]); out[used] = '\0';
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    return !bad && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int inside_audio(Db *db, const char *path)
{
    size_t n = strlen(db->audio_directory);
    if (n == 1 && db->audio_directory[0] == '/') return path[0] == '/';
    return !strncmp(path, db->audio_directory, n) && path[n] == '/';
}

static size_t utf8_characters(const char *text)
{
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xc0) != 0x80) ++count;
    return count;
}

static DbResult read_metadata(Db *db, const char *path, SongInfo *song)
{
    struct stat before, after;
    if (stat(path, &before) || !S_ISREG(before.st_mode)) return error(db, DB_FILE_ERROR, "Not a regular MP3");
    char output[8192];
    char *args[] = {"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
        "stream=codec_name,sample_rate,channels:format=duration", "-of",
        "default=noprint_wrappers=1", (char *)path, NULL};
    if (capture(args, output, sizeof(output))) return error(db, DB_FILE_ERROR, "ffprobe failed");
    memset(song, 0, sizeof(*song));
    double duration = -1;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "codec_name=", 11)) copy_text(song->codec, sizeof(song->codec), line + 11);
        if (!strncmp(line, "sample_rate=", 12)) song->sample_rate_hz = atoi(line + 12);
        if (!strncmp(line, "channels=", 9)) song->channel_count = atoi(line + 9);
        if (!strncmp(line, "duration=", 9)) {
            char *end; duration = strtod(line + 9, &end); if (*end) duration = -1;
        }
    }
    if (strcmp(song->codec, "mp3") || song->sample_rate_hz <= 0 || song->channel_count <= 0
            || !isfinite(duration) || duration <= 0 || duration >= (double)LLONG_MAX / 1000)
        return error(db, DB_FILE_ERROR, "Invalid MP3 metadata");
    strcpy(song->codec, "MP3");
    song->duration = (long long)llround(duration * 1000);
    char *hash[] = {"sha256sum", "--", (char *)path, NULL};
    if (capture(hash, output, sizeof(output))) return error(db, DB_FILE_ERROR, "sha256sum failed");
    const char *hex = output + (output[0] == '\\');
    if (strlen(hex) < 64) return error(db, DB_FILE_ERROR, "Invalid checksum");
    for (int i = 0; i < 64; ++i) if (!isxdigit((unsigned char)hex[i]))
        return error(db, DB_FILE_ERROR, "Invalid checksum");
    memcpy(song->checksum_sha256, hex, 64);
    if (stat(path, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino
            || before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
            || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
        return error(db, DB_CONFLICT, "File changed during metadata scan; retry after copying finishes");
    song->file_size_bytes = (long long)after.st_size;
    if (copy_text(song->file_path, sizeof(song->file_path), path))
        return error(db, DB_INVALID_ARGUMENT, "Path exceeds API capacity");
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length <= 4 || strcasecmp(name + length - 4, ".mp3") || length >= sizeof(song->title))
        return error(db, DB_INVALID_ARGUMENT, "Expected .mp3 filename");
    memcpy(song->title, name, length - 4);
    char *suffix = strrchr(song->title, '[');
    if (suffix && suffix > song->title && suffix[-1] == ' ' && strlen(suffix) == 13 && suffix[12] == ']') {
        int valid = 1;
        for (int i = 1; i <= 11; ++i) if (!(isalnum((unsigned char)suffix[i]) || suffix[i] == '-' || suffix[i] == '_')) valid = 0;
        if (valid) suffix[-1] = '\0';
    }
    if (!*song->title) return error(db, DB_INVALID_ARGUMENT, "Empty title");
    if (utf8_characters(song->title) > 200 || utf8_characters(path) > 500)
        return error(db, DB_INVALID_ARGUMENT, "Title/path exceeds Song column length");
    return DB_OK;
}

static DbResult insert_song_locked(Db *db, const char *file_path, long long *song_id)
{
    if (!file_path || !song_id) return error(db, DB_INVALID_ARGUMENT, "Path and output ID required");
    *song_id = 0;
    char path[PATH_MAX];
    if (!realpath(file_path, path)) return error(db, DB_FILE_ERROR, "%s", strerror(errno));
    if (!inside_audio(db, path)) return error(db, DB_INVALID_ARGUMENT, "MP3 must be inside configured audio directory");
    SongInfo song;
    DbResult status = read_metadata(db, path, &song);
    if (status != DB_OK) return status;
    char *title = text_value(song.title), *file = text_value(path), *hash = text_value(song.checksum_sha256);
    if (!title || !file || !hash) { free(title); free(file); free(hash); return error(db, DB_MEMORY_ERROR, "Cannot allocate SQL values"); }
    status = query(db, "SELECT song_id,file_path FROM Song WHERE file_path=%s OR checksum_sha256=%s "
                      "ORDER BY (file_path<>%s) DESC,song_id LIMIT 1", file, hash, file);
    if (status != DB_OK) goto finished;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) goto finished;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row) {
        *song_id = atoll(row[0]);
        int same_path = !strcmp(row[1], path);
        mysql_free_result(result);
        if (!same_path) { status = error(db, DB_ALREADY_EXISTS, "Identical MP3 already registered as song %lld", *song_id); goto finished; }
        status = query(db, "UPDATE Song SET codec='MP3',sample_rate_hz=%d,channel_count=%d,"
            "duration=%lld,file_size_bytes=%lld,checksum_sha256=%s WHERE song_id=%lld",
            song.sample_rate_hz, song.channel_count, song.duration, song.file_size_bytes, hash, *song_id);
    } else {
        mysql_free_result(result);
        status = query(db, "INSERT INTO Song(title,file_path,codec,sample_rate_hz,channel_count,"
            "duration,file_size_bytes,checksum_sha256) VALUES(%s,%s,'MP3',%d,%d,%lld,%lld,%s)",
            title, file, song.sample_rate_hz, song.channel_count, song.duration, song.file_size_bytes, hash);
        if (status == DB_OK) *song_id = (long long)mysql_insert_id(db->connection);
        else if (mysql_errno(db->connection) == 1062)
            status = error(db, DB_CONFLICT, "File path was registered concurrently; retry");
    }
finished:
    free(title); free(file); free(hash);
    return status;
}

static DbResult delete_song_locked(Db *db, long long song_id)
{
    if (song_id <= 0) return error(db, DB_INVALID_ARGUMENT, "Positive song ID required");
    int reserved = 0;
    if (!db->offline) {
        if (!db->runtime.reserve_song_delete || !db->runtime.release_song_delete)
            return error(db, DB_RUNTIME_UNAVAILABLE, "Song deletion needs server reservation hooks, or offline management");
        int result = db->runtime.reserve_song_delete(db->runtime.context, song_id);
        if (result) return error(db, result == 1 ? DB_BUSY : DB_RUNTIME_UNAVAILABLE, "Song cannot be reserved for deletion");
        reserved = 1;
    }
    DbResult status = query(db, "START TRANSACTION");
    char path[PATH_MAX] = "", staged[PATH_MAX] = "";
    int moved = 0;
    if (status != DB_OK) goto release;
    status = query(db, "SELECT file_path FROM Song WHERE song_id=%lld FOR UPDATE", song_id);
    if (status != DB_OK) goto rollback;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) goto rollback;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); status = error(db, DB_NOT_FOUND, "Song not found"); goto rollback; }
    if (copy_text(path, sizeof(path), row[0])) {
        mysql_free_result(result); status = error(db, DB_FILE_ERROR, "Path too long"); goto rollback;
    }
    mysql_free_result(result);
    char canonical[PATH_MAX];
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || !realpath(path, canonical)
            || !inside_audio(db, canonical)) {
        status = error(db, DB_FILE_ERROR, "Song file missing, not regular, or outside audio directory"); goto rollback;
    }
    int length = snprintf(staged, sizeof(staged), "%s.delete-XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(staged)) {
        status = error(db, DB_FILE_ERROR, "Staging path too long"); goto rollback;
    }
    int fd = mkstemp(staged);
    if (fd < 0) { status = error(db, DB_FILE_ERROR, "Cannot stage file: %s", strerror(errno)); goto rollback; }
    close(fd);
    if (rename(path, staged)) {
        int saved = errno; unlink(staged);
        status = error(db, DB_FILE_ERROR, "Cannot move MP3: %s", strerror(saved)); goto rollback;
    }
    moved = 1;
    status = query(db, "DELETE FROM Song WHERE song_id=%lld", song_id);
    if (status != DB_OK) goto rollback;
    if (mysql_commit(db->connection)) {
        /* Connection loss can leave COMMIT outcome unknown: retain the file. */
        status = error(db, DB_RECOVERY_REQUIRED, "COMMIT outcome unknown. Check DB song %lld; retained MP3: %s", song_id, staged);
        goto release;
    }
    if (unlink(staged)) status = error(db, DB_CLEANUP_PENDING, "DB row deleted; remove retained file manually: %s", staged);
    else status = DB_OK;
    goto release;
rollback:
    if (mysql_rollback(db->connection)) {
        status = error(db, DB_RECOVERY_REQUIRED, "Rollback failed. Check DB song %lld; file location: %s", song_id, moved ? staged : path);
    } else if (moved) {
        /* Restore without replacing a file another process may have created. */
        if (link(staged, path)) status = error(db, DB_RECOVERY_REQUIRED, "Restore failed. Retained MP3: %s", staged);
        else if (unlink(staged)) status = error(db, DB_RECOVERY_REQUIRED, "MP3 restored; extra staged file remains: %s", staged);
    }
release:
    if (reserved) db->runtime.release_song_delete(db->runtime.context, song_id);
    return status;
}

/* Serialize song writes across processes using this API. This also prevents
 * two different paths with identical hashes racing through duplicate checks.
 */
static DbResult lock_song_catalog(Db *db)
{
    DbResult status = query(db, "SELECT GET_LOCK('speaker_song_catalog_write_v1',5)");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    int acquired = row && row[0] && !strcmp(row[0], "1");
    mysql_free_result(result);
    return acquired ? DB_OK : error(db, DB_BUSY, "Another song catalog operation is running; retry");
}

static DbResult unlock_song_catalog(Db *db, DbResult status)
{
    char saved[sizeof(db->error)];
    strcpy(saved, db->error);
    DbResult released = query(db, "DO RELEASE_LOCK('speaker_song_catalog_write_v1')");
    if (released != DB_OK && status == DB_OK)
        return error(db, DB_RECOVERY_REQUIRED, "Song operation completed, but lock release failed; reconnect and inspect DB");
    strcpy(db->error, saved);
    return status;
}

DbResult insert_song(Db *db, const char *file_path, long long *song_id)
{
    if (song_id) *song_id = 0;
    DbResult status = lock_song_catalog(db);
    if (status != DB_OK) return status;
    status = insert_song_locked(db, file_path, song_id);
    return unlock_song_catalog(db, status);
}

DbResult delete_song(Db *db, long long song_id)
{
    DbResult status = lock_song_catalog(db);
    if (status != DB_OK) return status;
    status = delete_song_locked(db, song_id);
    return unlock_song_catalog(db, status);
}
