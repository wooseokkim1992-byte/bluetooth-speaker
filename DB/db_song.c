#define _GNU_SOURCE
#include "db_internal.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

DbResult list_song(Db *db, SongInfo **out, size_t *count)
{
    if (!db || !out || !count) return db_set_error(db, DB_INVALID_ARGUMENT, "Output required");
    *out = NULL; *count = 0;
    DbResult status = db_query(db,
        "SELECT song_id,title,file_path,codec,duration,file_size_bytes,checksum_sha256 FROM Song ORDER BY song_id");
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    void *items;
    status = db_allocate_list(db, result, sizeof(**out), &items);
    if (status == DB_OK) {
        *out = items;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            SongInfo *song = &(*out)[*count];
            song->song_id = atoll(row[0]);
            song->duration = atoll(row[4]);
            song->file_size_bytes = atoll(row[5]);
            if (db_copy_text(song->title, sizeof(song->title), row[1])
                    || db_copy_text(song->file_path, sizeof(song->file_path), row[2])
                    || db_copy_text(song->codec, sizeof(song->codec), row[3])
                    || db_copy_text(song->checksum_sha256, sizeof(song->checksum_sha256), row[6])) {
                status = db_set_error(db, DB_DATABASE_ERROR, "Invalid Song data"); break;
            }
            ++*count;
        }
    }
    mysql_free_result(result);
    if (status != DB_OK) { free(*out); *out = NULL; *count = 0; }
    return status;
}

/* Runs ffprobe/sha256sum and reads stdout. execvp passes the filename as a
 * single argument: quotes/spaces in song names do not become shell commands.
 */
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

static DbResult read_metadata(Db *db, const char *path, SongInfo *song)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode))
        return db_set_error(db, DB_FILE_ERROR, "Not a regular MP3 file");
    memset(song, 0, sizeof(*song));
    char output[4096], codec[65] = "";
    char *probe[] = {"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
        "stream=codec_name:format=duration", "-of", "default=noprint_wrappers=1", (char *)path, NULL};
    if (capture(probe, output, sizeof(output)))
        return db_set_error(db, DB_FILE_ERROR, "ffprobe failed");
    double duration = -1;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "codec_name=", 11)) db_copy_text(codec, sizeof(codec), line + 11);
        if (!strncmp(line, "duration=", 9)) {
            char *end;
            duration = strtod(line + 9, &end);
            if (*end) duration = -1;
        }
    }
    if (strcmp(codec, "mp3") || !isfinite(duration) || duration <= 0 || duration >= (double)LLONG_MAX)
        return db_set_error(db, DB_FILE_ERROR, "Invalid MP3 duration/codec");
    strcpy(song->codec, "MP3");
    song->duration = (long long)duration;
    song->file_size_bytes = (long long)st.st_size;
    if (db_copy_text(song->file_path, sizeof(song->file_path), path))
        return db_set_error(db, DB_INVALID_ARGUMENT, "File path too long");
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length <= 4 || strcasecmp(name + length - 4, ".mp3") || length - 4 >= sizeof(song->title))
        return db_set_error(db, DB_INVALID_ARGUMENT, "Expected .mp3 filename");
    memcpy(song->title, name, length - 4); /* Use filename without extension as title. */
    size_t title_chars = 0, path_chars = 0;
    for (const unsigned char *p = (const unsigned char *)song->title; *p; ++p)
        if ((*p & 0xc0) != 0x80) ++title_chars;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if ((*p & 0xc0) != 0x80) ++path_chars;
    if (title_chars > 200 || path_chars > 500)
        return db_set_error(db, DB_INVALID_ARGUMENT, "Title/path exceeds DB column length");

    char *checksum[] = {"sha256sum", "--", (char *)path, NULL};
    if (capture(checksum, output, sizeof(output)))
        return db_set_error(db, DB_FILE_ERROR, "sha256sum failed");
    const char *hex = output + (output[0] == '\\'); /* Escaped filename output. */
    if (strlen(hex) < 64) return db_set_error(db, DB_FILE_ERROR, "Invalid checksum");
    for (size_t i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)hex[i])) return db_set_error(db, DB_FILE_ERROR, "Invalid checksum");
    memcpy(song->checksum_sha256, hex, 64);
    return DB_OK;
}

DbResult insert_song(Db *db, const char *file_path, long long *song_id)
{
    if (song_id) *song_id = 0;
    if (!db || !file_path || !song_id)
        return db_set_error(db, DB_INVALID_ARGUMENT, "File path/output required");
    char path[PATH_MAX];
    if (!realpath(file_path, path)) return db_set_error(db, DB_FILE_ERROR, "%s", strerror(errno));
    SongInfo song;
    DbResult status = read_metadata(db, path, &song);
    if (status != DB_OK) return status;
    char *title = db_text_value(song.title), *file = db_text_value(path);
    if (!title || !file) { free(title); free(file); return db_set_error(db, DB_MEMORY_ERROR, "Cannot allocate values"); }
    status = db_query(db,
        "INSERT INTO Song(title,file_path,codec,duration,file_size_bytes,checksum_sha256) "
        "VALUES(%s,%s,'MP3',%lld,%lld,'%s')",
        title, file, song.duration, song.file_size_bytes, song.checksum_sha256);
    free(title); free(file);
    if (status == DB_DATABASE_ERROR && mysql_errno(db->connection) == 1062)
        return db_set_error(db, DB_ALREADY_EXISTS, "Song file path already registered");
    if (status == DB_OK) *song_id = (long long)mysql_insert_id(db->connection);
    return status;
}

DbResult delete_song(Db *db, long long song_id)
{
    if (!db || song_id <= 0) return db_set_error(db, DB_INVALID_ARGUMENT, "Positive song ID required");
    DbResult status = db_query(db, "SELECT file_path FROM Song WHERE song_id=%lld", song_id);
    if (status != DB_OK) return status;
    MYSQL_RES *result;
    if ((status = db_get_result(db, &result)) != DB_OK) return status;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return db_set_error(db, DB_NOT_FOUND, "Song not found"); }
    char path[2001];
    int bad = db_copy_text(path, sizeof(path), row[0]);
    mysql_free_result(result);
    if (bad) return db_set_error(db, DB_FILE_ERROR, "Invalid MP3 path");
    /* Caller ensures the server is no longer using the song.
     * No staging, locks or transaction; missing files allow removing stale rows.
     */
    if (unlink(path) && errno != ENOENT)
        return db_set_error(db, DB_FILE_ERROR, "MP3 deletion failed: %s", strerror(errno));
    status = db_query(db, "DELETE FROM Song WHERE song_id=%lld", song_id);
    if (status != DB_OK) {
        char error[512];
        snprintf(error, sizeof(error), "%s", db_error(db));
        return db_set_error(db, status, "MP3 removed, but DB deletion failed: %s", error);
    }
    return DB_OK;
}
