#define _GNU_SOURCE
#include <mysql.h>
#include <ctype.h>
#include <dirent.h>
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

/* One-time build:
 * gcc -std=c11 -Wall -Wextra -O2 song_import.c -o song_import \
 *     $(mariadb_config --cflags --libs) -lm
 * Run: sudo ./song_import [absolute MP3 directory]
 * Requires ffprobe (ffmpeg package) and sha256sum (coreutils).
 * Files stay in their directory. Song stores metadata, not MP3 binary data.
 */

static int capture(char *const args[], char *out, size_t capacity)
{
    int pipes[2], status;
    if (pipe(pipes) != 0) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (child == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipes[1]);
        execvp(args[0], args);
        perror(args[0]);
        _exit(127);
    }
    close(pipes[1]);
    size_t used = 0;
    int bad = 0;
    char block[1024];
    ssize_t count;
    while ((count = read(pipes[0], block, sizeof(block))) != 0) {
        if (count < 0) {
            if (errno == EINTR) continue;
            bad = 1; break;
        }
        if ((size_t)count > capacity - used - 1) bad = 1;
        else { memcpy(out + used, block, (size_t)count); used += (size_t)count; }
    }
    close(pipes[0]);
    out[used] = '\0';
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return !bad && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void make_title(char *title, const char *filename)
{
    size_t length = strlen(filename);
    memcpy(title, filename, length - 4);
    title[length - 4] = '\0';
    /* Remove a trailing [11-character video ID] from the DB title only. */
    char *suffix = strrchr(title, '[');
    if (suffix && suffix > title && suffix[-1] == ' ' && strlen(suffix) == 13
            && suffix[12] == ']') {
        int valid = 1;
        for (int i = 1; i <= 11; ++i) {
            unsigned char c = (unsigned char)suffix[i];
            if (!(isalnum(c) || c == '_' || c == '-')) valid = 0;
        }
        if (valid) suffix[-1] = '\0';
    }
}

/* Return 1 if identical audio is already registered at another path. */
static int duplicate_audio(MYSQL *db, const char *checksum, const char *path)
{
    const char *sql = "SELECT song_id FROM Song WHERE checksum_sha256=? "
                      "AND file_path<>? LIMIT 1";
    MYSQL_STMT *check = mysql_stmt_init(db);
    if (!check) return -1;
    MYSQL_BIND bindings[2] = {{0}};
    const char *values[2] = {checksum, path};
    unsigned long lengths[2];
    for (int i = 0; i < 2; ++i) {
        lengths[i] = (unsigned long)strlen(values[i]);
        bindings[i].buffer_type = MYSQL_TYPE_STRING;
        bindings[i].buffer = (void *)values[i];
        bindings[i].buffer_length = lengths[i];
        bindings[i].length = &lengths[i];
    }
    int result = -1;
    if (mysql_stmt_prepare(check, sql, (unsigned long)strlen(sql)) == 0
            && mysql_stmt_bind_param(check, bindings) == 0
            && mysql_stmt_execute(check) == 0
            && mysql_stmt_store_result(check) == 0)
        result = mysql_stmt_num_rows(check) != 0;
    else fprintf(stderr, "Duplicate check failed: %s\n", mysql_stmt_error(check));
    mysql_stmt_close(check);
    return result;
}

static int register_song(MYSQL *db, MYSQL_STMT *statement, const char *path,
                         const char *filename, const struct stat *before)
{
    char text[8192], title[NAME_MAX + 1], checksum[65];
    char rate[32] = "", channels[32] = "", duration_ms[32], size[32];
    char *probe[] = {"ffprobe", "-v", "error", "-select_streams", "a:0",
        "-show_entries", "stream=codec_name,sample_rate,channels:format=duration",
        "-of", "default=noprint_wrappers=1", (char *)path, NULL};
    if (capture(probe, text, sizeof(text)) != 0) return -1;
    double duration = -1;
    int is_mp3 = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(line, "codec_name=mp3")) is_mp3 = 1;
        if (!strncmp(line, "sample_rate=", 12))
            snprintf(rate, sizeof(rate), "%s", line + 12);
        if (!strncmp(line, "channels=", 9))
            snprintf(channels, sizeof(channels), "%s", line + 9);
        if (!strncmp(line, "duration=", 9)) {
            char *end;
            duration = strtod(line + 9, &end);
            if (*end) duration = -1;
        }
    }
    if (!is_mp3 || atoi(rate) <= 0 || atoi(channels) <= 0
            || !isfinite(duration) || duration <= 0
            || duration >= (double)LLONG_MAX / 1000.0) {
        fprintf(stderr, "Invalid MP3 metadata: %s\n", path);
        return -1;
    }
    char *hash[] = {"sha256sum", "--", (char *)path, NULL};
    if (capture(hash, text, sizeof(text)) != 0) return -1;
    const char *hex = text + (text[0] == '\\');
    if (strlen(hex) < 64) return -1;
    for (int i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)hex[i])) return -1;
    memcpy(checksum, hex, 64); checksum[64] = '\0';
    struct stat after;
    if (stat(path, &after) != 0 || before->st_dev != after.st_dev
            || before->st_ino != after.st_ino || before->st_size != after.st_size
            || before->st_mtim.tv_sec != after.st_mtim.tv_sec
            || before->st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        fprintf(stderr, "File changed during scan; retry after copying finishes: %s\n", path);
        return -1;
    }
    int duplicate = duplicate_audio(db, checksum, path);
    if (duplicate < 0) return -1;
    if (duplicate) {
        printf("Skipped identical audio already registered: %s\n", path);
        return 1;
    }
    make_title(title, filename);
    if (!*title) return -1;
    snprintf(duration_ms, sizeof(duration_ms), "%lld", (long long)llround(duration * 1000));
    snprintf(size, sizeof(size), "%lld", (long long)after.st_size);
    const char *values[] = {title, path, "MP3", rate, channels,
                           duration_ms, size, checksum};
    MYSQL_BIND bindings[8] = {{0}};
    unsigned long lengths[8];
    for (int i = 0; i < 8; ++i) {
        lengths[i] = (unsigned long)strlen(values[i]);
        bindings[i].buffer_type = MYSQL_TYPE_STRING;
        bindings[i].buffer = (void *)values[i];
        bindings[i].buffer_length = lengths[i];
        bindings[i].length = &lengths[i];
    }
    if (mysql_stmt_bind_param(statement, bindings) != 0
            || mysql_stmt_execute(statement) != 0) {
        fprintf(stderr, "DB write failed: %s\n", mysql_stmt_error(statement));
        return -1;
    }
    printf("Registered: %s (%s Hz, %s channels, %s ms)\n",
           title, rate, channels, duration_ms);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) { fprintf(stderr, "Usage: %s [MP3-directory]\n", argv[0]); return 1; }
    char directory[PATH_MAX];
    if (!realpath(argc == 2 ? argv[1] : "/home/jetson/speaker_stream/base", directory)) {
        perror("MP3 directory"); return 1;
    }
    DIR *folder = opendir(directory);
    if (!folder) { perror(directory); return 1; }
    MYSQL *db = mysql_init(NULL);
    if (!db) { closedir(folder); return 1; }
    if (!mysql_real_connect(db, "localhost", "root", NULL, "speaker_stream", 0, NULL, 0)
            || mysql_set_character_set(db, "utf8mb4") != 0) {
        fprintf(stderr, "DB connection failed: %s\n", mysql_error(db));
        mysql_close(db); closedir(folder); return 1;
    }
    const char *sql =
        "INSERT INTO Song (title,file_path,codec,sample_rate_hz,channel_count,"
        "duration_ms,file_size_bytes,checksum_sha256) VALUES (?,?,?,?,?,?,?,?) "
        "ON DUPLICATE KEY UPDATE codec=VALUES(codec),"
        "sample_rate_hz=VALUES(sample_rate_hz),channel_count=VALUES(channel_count),"
        "duration_ms=VALUES(duration_ms),file_size_bytes=VALUES(file_size_bytes),"
        "checksum_sha256=VALUES(checksum_sha256)";
    MYSQL_STMT *statement = mysql_stmt_init(db);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) {
        fprintf(stderr, "DB prepare failed: %s\n",
                statement ? mysql_stmt_error(statement) : mysql_error(db));
        if (statement) mysql_stmt_close(statement);
        mysql_close(db); closedir(folder); return 1;
    }
    unsigned imported = 0, skipped = 0, failed = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(folder);
        if (!entry) {
            if (errno) { perror("Directory scan"); ++failed; }
            break;
        }
        size_t length = strlen(entry->d_name);
        if (length <= 4 || strcasecmp(entry->d_name + length - 4, ".mp3")) continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { ++failed; continue; }
        struct stat st;
        if (lstat(path, &st) != 0) { perror(path); ++failed; continue; }
        if (!S_ISREG(st.st_mode)) continue;
        int registered = register_song(db, statement, path, entry->d_name, &st);
        if (registered == 0) ++imported;
        else if (registered == 1) ++skipped;
        else { fprintf(stderr, "Failed: %s\n", path); ++failed; }
    }
    mysql_stmt_close(statement);
    mysql_close(db);
    closedir(folder);
    printf("Finished: %u registered/updated, %u duplicates skipped, %u failed\n",
           imported, skipped, failed);
    return failed ? 1 : 0;
}

