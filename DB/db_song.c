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

static int parse_song(MYSQL_ROW row, SongInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->song_id = atoll(row[0]);
    out->sample_rate_hz = atoi(row[4]);
    out->channel_count = atoi(row[5]);
    out->duration = atoll(row[6]);
    out->file_size_bytes = atoll(row[7]);
    return db_copy_text(out->title, sizeof(out->title), row[1])
        || db_copy_text(out->file_path, sizeof(out->file_path), row[2])
        || db_copy_text(out->codec, sizeof(out->codec), row[3])
        || db_copy_text(out->checksum_sha256, sizeof(out->checksum_sha256), row[8]);
}

DbResult list_song(Db *db, SongInfo **out, size_t *count)
{
    if (!out || !count) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = db_query(db, "SELECT song_id,title,file_path,codec,sample_rate_hz,"
        "channel_count,duration,file_size_bytes,checksum_sha256 FROM Song ORDER BY song_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items, count);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (parse_song(row, &(*out)[*count])) { status = db_set_error(db, DB_DATABASE_ERROR, "Invalid Song data"); break; }
            ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; }
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
    if (stat(path, &before) || !S_ISREG(before.st_mode)) return db_set_error(db, DB_FILE_ERROR, "Not a regular MP3");
    char output[8192];
    char *args[] = {"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
        "stream=codec_name,sample_rate,channels:format=duration", "-of",
        "default=noprint_wrappers=1", (char *)path, NULL};
    if (capture(args, output, sizeof(output))) return db_set_error(db, DB_FILE_ERROR, "ffprobe failed");
    memset(song, 0, sizeof(*song));
    double duration = -1;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "codec_name=", 11)) db_copy_text(song->codec, sizeof(song->codec), line + 11);
        if (!strncmp(line, "sample_rate=", 12)) song->sample_rate_hz = atoi(line + 12);
        if (!strncmp(line, "channels=", 9)) song->channel_count = atoi(line + 9);
        if (!strncmp(line, "duration=", 9)) {
            char *end; duration = strtod(line + 9, &end); if (*end) duration = -1;
        }
    }
    if (strcmp(song->codec, "mp3") || song->sample_rate_hz <= 0 || song->channel_count <= 0
            || !isfinite(duration) || duration <= 0 || duration >= (double)LLONG_MAX)
        return db_set_error(db, DB_FILE_ERROR, "Invalid MP3 metadata");
    strcpy(song->codec, "MP3");
    song->duration = (long long)duration; /* Whole seconds, matching the DB column. */
    char *hash[] = {"sha256sum", "--", (char *)path, NULL};
    if (capture(hash, output, sizeof(output))) return db_set_error(db, DB_FILE_ERROR, "sha256sum failed");
    const char *hex = output + (output[0] == '\\');
    if (strlen(hex) < 64) return db_set_error(db, DB_FILE_ERROR, "Invalid checksum");
    for (int i = 0; i < 64; ++i) if (!isxdigit((unsigned char)hex[i]))
        return db_set_error(db, DB_FILE_ERROR, "Invalid checksum");
    memcpy(song->checksum_sha256, hex, 64);
    if (stat(path, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino
            || before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
            || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
        return db_set_error(db, DB_CONFLICT, "File changed during metadata scan; retry after copying finishes");
    song->file_size_bytes = (long long)after.st_size;
    if (db_copy_text(song->file_path, sizeof(song->file_path), path))
        return db_set_error(db, DB_INVALID_ARGUMENT, "Path exceeds API capacity");
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length <= 4 || strcasecmp(name + length - 4, ".mp3") || length >= sizeof(song->title))
        return db_set_error(db, DB_INVALID_ARGUMENT, "Expected .mp3 filename");
    memcpy(song->title, name, length - 4);
    char *suffix = strrchr(song->title, '[');
    if (suffix && suffix > song->title && suffix[-1] == ' ' && strlen(suffix) == 13 && suffix[12] == ']') {
        int valid = 1;
        for (int i = 1; i <= 11; ++i) if (!(isalnum((unsigned char)suffix[i]) || suffix[i] == '-' || suffix[i] == '_')) valid = 0;
        if (valid) suffix[-1] = '\0';
    }
    if (!*song->title) return db_set_error(db, DB_INVALID_ARGUMENT, "Empty title");
    if (utf8_characters(song->title) > 200 || utf8_characters(path) > 500)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Title/path exceeds Song column length");
    return DB_OK;
}

static DbResult insert_song_locked(Db *db, const char *file_path, long long *song_id)
{
    if (!file_path || !song_id) return db_set_error(db, DB_INVALID_ARGUMENT, "Path and output ID required");
    *song_id = 0;
    char path[PATH_MAX];
    if (!realpath(file_path, path)) return db_set_error(db, DB_FILE_ERROR, "%s", strerror(errno));
    if (!inside_audio(db, path)) return db_set_error(db, DB_INVALID_ARGUMENT, "MP3 must be inside configured audio directory");
    SongInfo song;
    DbResult status = read_metadata(db, path, &song);
    if (status != DB_OK) return status;
    char *title = db_text_value(song.title), *file = db_text_value(path), *hash = db_text_value(song.checksum_sha256);
    if (!title || !file || !hash) { free(title); free(file); free(hash); return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate SQL values"); }
    status = db_query(db, "SELECT song_id,file_path FROM Song WHERE file_path=%s OR checksum_sha256=%s "
                      "ORDER BY (file_path<>%s) DESC,song_id LIMIT 1", file, hash, file);
    if (status != DB_OK) goto finished;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) goto finished;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row) {
        *song_id = atoll(row[0]);
        int same_path = !strcmp(row[1], path);
        mysql_free_result(result);
        if (!same_path) { status = db_set_error(db, DB_ALREADY_EXISTS, "Identical MP3 already registered as song %lld", *song_id); goto finished; }
        status = db_query(db, "UPDATE Song SET codec='MP3',sample_rate_hz=%d,channel_count=%d,"
            "duration=%lld,file_size_bytes=%lld,checksum_sha256=%s WHERE song_id=%lld",
            song.sample_rate_hz, song.channel_count, song.duration, song.file_size_bytes, hash, *song_id);
    } else {
        mysql_free_result(result);
        status = db_query(db, "INSERT INTO Song(title,file_path,codec,sample_rate_hz,channel_count,"
            "duration,file_size_bytes,checksum_sha256) VALUES(%s,%s,'MP3',%d,%d,%lld,%lld,%s)",
            title, file, song.sample_rate_hz, song.channel_count, song.duration, song.file_size_bytes, hash);
        if (status == DB_OK) *song_id = (long long)mysql_insert_id(db->connection);
        else if (mysql_errno(db->connection) == 1062)
            status = db_set_error(db, DB_CONFLICT, "File path was registered concurrently; retry");
    }
finished:
    free(title); free(file); free(hash);
    return status;
}

static DbResult delete_song_locked(Db *db, long long song_id)
{
    if (song_id <= 0) return db_set_error(db, DB_INVALID_ARGUMENT, "Positive song ID required");
    int reserved = 0;
    if (!db->offline) {
        if (!db->runtime.reserve_song_delete || !db->runtime.release_song_delete)
            return db_set_error(db, DB_RUNTIME_UNAVAILABLE, "Song deletion needs server reservation hooks, or offline management");
        int result = db->runtime.reserve_song_delete(db->runtime.context, song_id);
        if (result) return db_set_error(db, result == 1 ? DB_BUSY : DB_RUNTIME_UNAVAILABLE, "Song cannot be reserved for deletion");
        reserved = 1;
    }
    DbResult status = db_query(db, "START TRANSACTION");
    char path[PATH_MAX] = "", staged[PATH_MAX] = "";
    int moved = 0;
    if (status != DB_OK) goto release;
    status = db_query(db, "SELECT file_path FROM Song WHERE song_id=%lld FOR UPDATE", song_id);
    if (status != DB_OK) goto rollback;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) goto rollback;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); status = db_set_error(db, DB_NOT_FOUND, "Song not found"); goto rollback; }
    if (db_copy_text(path, sizeof(path), row[0])) {
        mysql_free_result(result); status = db_set_error(db, DB_FILE_ERROR, "Path too long"); goto rollback;
    }
    mysql_free_result(result);
    char canonical[PATH_MAX];
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || !realpath(path, canonical)
            || !inside_audio(db, canonical)) {
        status = db_set_error(db, DB_FILE_ERROR, "Song file missing, not regular, or outside audio directory"); goto rollback;
    }
    int length = snprintf(staged, sizeof(staged), "%s.delete-XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(staged)) {
        status = db_set_error(db, DB_FILE_ERROR, "Staging path too long"); goto rollback;
    }
    int fd = mkstemp(staged);
    if (fd < 0) { status = db_set_error(db, DB_FILE_ERROR, "Cannot stage file: %s", strerror(errno)); goto rollback; }
    close(fd);
    if (rename(path, staged)) {
        int saved = errno; unlink(staged);
        status = db_set_error(db, DB_FILE_ERROR, "Cannot move MP3: %s", strerror(saved)); goto rollback;
    }
    moved = 1;
    status = db_query(db, "DELETE FROM Song WHERE song_id=%lld", song_id);
    if (status != DB_OK) goto rollback;
    if (mysql_commit(db->connection)) {
        /* Connection loss can leave COMMIT outcome unknown: retain the file. */
        status = db_set_error(db, DB_RECOVERY_REQUIRED, "COMMIT outcome unknown. Check DB song %lld; retained MP3: %s", song_id, staged);
        goto release;
    }
    if (unlink(staged)) status = db_set_error(db, DB_CLEANUP_PENDING, "DB row deleted; remove retained file manually: %s", staged);
    else status = DB_OK;
    goto release;
rollback:
    if (mysql_rollback(db->connection)) {
        status = db_set_error(db, DB_RECOVERY_REQUIRED, "Rollback failed. Check DB song %lld; file location: %s", song_id, moved ? staged : path);
    } else if (moved) {
        /* Restore without replacing a file another process may have created. */
        if (link(staged, path)) status = db_set_error(db, DB_RECOVERY_REQUIRED, "Restore failed. Retained MP3: %s", staged);
        else if (unlink(staged)) status = db_set_error(db, DB_RECOVERY_REQUIRED, "MP3 restored; extra staged file remains: %s", staged);
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
    DbResult status = db_query(db, "SELECT GET_LOCK('speaker_song_catalog_write_v1',5)");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    int acquired = row && row[0] && !strcmp(row[0], "1");
    mysql_free_result(result);
    return acquired ? DB_OK : db_set_error(db, DB_BUSY, "Another song catalog operation is running; retry");
}

static DbResult unlock_song_catalog(Db *db, DbResult status)
{
    char saved[sizeof(db->error)];
    strcpy(saved, db->error);
    DbResult released = db_query(db, "DO RELEASE_LOCK('speaker_song_catalog_write_v1')");
    if (released != DB_OK && status == DB_OK)
        return db_set_error(db, DB_RECOVERY_REQUIRED, "Song operation completed, but lock release failed; reconnect and inspect DB");
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
