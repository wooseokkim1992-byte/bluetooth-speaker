#define _GNU_SOURCE
#include "db_api.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static int positive_number(const char *text, long long *out)
{
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (!isdigit(*p)) return 0;
    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || *end || value <= 0) return 0;
    *out = value;
    return 1;
}

typedef enum { ARG_NONE, ARG_DEVICE_ID, ARG_DEVICE, ARG_SONG_ID, ARG_FILE } ArgumentKind;

typedef struct {
    const char *name, *syntax;
    int argument_count;
    ArgumentKind kind;
} Command;

static const Command commands[] = {
    {"insert_device", "insert_device device_id plan_id", 2, ARG_DEVICE},
    {"delete_device", "delete_device device_id", 1, ARG_DEVICE_ID},
    {"update_plan", "update_plan device_id plan_id", 2, ARG_DEVICE},
    {"select_member", "select_member device_id", 1, ARG_DEVICE_ID},
    {"list_device", "list_device", 0, ARG_NONE},
    {"list_plan", "list_plan", 0, ARG_NONE},
    {"list_song", "list_song", 0, ARG_NONE},
    {"insert_song", "insert_song file_path", 1, ARG_FILE},
    {"delete_song", "delete_song song_id", 1, ARG_SONG_ID},
    {"scan_songs", "scan_songs", 0, ARG_NONE}
};

static const Command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(name, commands[i].name)) return &commands[i];
    return NULL;
}   

static int command_usage(const Command *command)
{
    fprintf(stderr, "%s\n", command->syntax);
    return 2;
}

static int device_number(const char *text, uint64_t *out)
{
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (!isdigit(*p)) return 0;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > UINT64_MAX) return 0;
    *out = (uint64_t)value;
    return 1;
}

/* Validate CLI syntax before opening the DB; runtime errors stay descriptive. */
static int valid_arguments(const Command *command, int count, const char *a, const char *b)
{
    if (count != command->argument_count) return 0;
    long long number;
    uint64_t device_id;
    switch (command->kind) {
    case ARG_NONE: return 1;
    case ARG_DEVICE_ID: return device_number(a, &device_id);
    case ARG_DEVICE:
        return device_number(a, &device_id) && positive_number(b, &number) && number <= INT_MAX;
    case ARG_SONG_ID: return positive_number(a, &number);
    case ARG_FILE: {
        size_t length = strlen(a);
        return length > 4 && !strcasecmp(a + length - 4, ".mp3");
    }
    }
    return 0;
}

int main(int argc, char **argv)
{
    DbConfig config = {
        .host = env_or("DB_HOST", "localhost"),
        .user = env_or("DB_USER", "root"),
        .password = getenv("DB_PASSWORD"),
        .database = env_or("DB_NAME", "speaker_stream"),
        .audio_directory = env_or("AUDIO_DIR", "/home/jetson/speaker_stream/base")
    };
    int position = 1;
    while (position < argc && !strncmp(argv[position], "--", 2)) {
        if (!strcmp(argv[position], "--offline")) { config.offline = 1; ++position; }
        else if (!strcmp(argv[position], "--audio-dir") && position + 1 < argc) {
            config.audio_directory = argv[position + 1]; position += 2;
        } else {
            fprintf(stderr, "Usage: %s [--offline] [--audio-dir PATH] COMMAND [arguments]\n", argv[0]);
            return 2;
        }
    }
    if (position == argc) {
        fprintf(stderr, "Usage: %s COMMAND [arguments]\n", argv[0]);
        return 2;
    }
    const char *command = argv[position++];
    if (!strcmp(command, "help")) {
        printf("%s COMMAND [arguments]\n", argv[0]);
        return 0;
    }
    int arguments = argc - position;
    const char *a = arguments > 0 ? argv[position] : NULL;
    const char *b = arguments > 1 ? argv[position + 1] : NULL;
    const Command *selected = find_command(command);
    if (!selected) {
        fprintf(stderr, "Unknown command: %s\n", command);
        return 2;
    }
    if (!valid_arguments(selected, arguments, a, b))
        return command_usage(selected);
    command = selected->name;
    Db *db = NULL;
    DbResult result = db_open(&db, &config, NULL);
    if (result != DB_OK) {
        fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db)); db_close(db); return 1;
    }
    long long number, id = 0;
    uint64_t device_id = 0;
    if (selected->kind == ARG_DEVICE || selected->kind == ARG_DEVICE_ID)
        device_number(a, &device_id);
    if (!strcmp(command, "insert_device") && arguments == 2) {
        result = positive_number(b, &number) && number <= INT_MAX
            ? insert_device(db, device_id, (int)number) : DB_INVALID_ARGUMENT;
    } else if (!strcmp(command, "delete_device") && arguments == 1) result = delete_device(db, device_id);
    else if (!strcmp(command, "update_plan") && arguments == 2) {
        result = positive_number(b, &number) && number <= INT_MAX
            ? update_plan(db, device_id, (int)number) : DB_INVALID_ARGUMENT;
    }
    else if (!strcmp(command, "select_member") && arguments == 1) {
        MemberInfo member;
        result = select_member(db, device_id, &member);
        if (result == DB_OK) {
            printf("Device ID: %llu\nStatus: %s\nPlan: %s (ID %d, %d bps)\n",
                (unsigned long long)member.device.device_id, member.device.status, member.plan.plan_code,
                member.plan.plan_id, member.plan.bitrate_bps);
            if (member.runtime_available) printf("Connected: %d\nPlayback: %s\nSong ID: %lld\nPlayed PTS(ms): %llu\n",
                member.runtime.connected, member.runtime.playback_state,
                member.runtime.current_song_id, member.runtime.last_played_pts_ms);
            else puts("Playback/connection/PTS: unavailable in standalone DB CLI; attach server runtime hooks.");
        }
    } else if (!strcmp(command, "list_device") && arguments == 0) {
        DeviceInfo *devices = NULL; size_t count = 0;
        result = list_device(db, &devices, &count);
        if (result == DB_OK) {
            for (size_t i = 0; i < count; ++i) printf("%llu | plan=%d | %s\n", (unsigned long long)devices[i].device_id, devices[i].plan_id, devices[i].status);
            printf("Total: %zu devices\n", count);
        }
        free(devices);
    } else if (!strcmp(command, "list_song") && arguments == 0) {
        SongInfo *songs = NULL; size_t count = 0;
        result = list_song(db, &songs, &count);
        if (result == DB_OK) {
            for (size_t i = 0; i < count; ++i) printf("%lld | %s | %lld분 %lld초\n", songs[i].song_id, songs[i].title, songs[i].duration / 60, songs[i].duration % 60);
            printf("Total: %zu songs\n", count);
        }
        free(songs);
    } else if (!strcmp(command, "list_plan") && arguments == 0) {
        PlanCount *plans = NULL; size_t count = 0; unsigned long long total = 0;
        result = list_plan(db, &plans, &count, &total);
        if (result == DB_OK) {
            for (size_t i = 0; i < count; ++i) printf("%s (ID %d) | members=%llu | active=%llu | %d bps\n",
                plans[i].plan.plan_code, plans[i].plan.plan_id, plans[i].member_count,
                plans[i].active_count, plans[i].plan.bitrate_bps);
            printf("Total registered devices: %llu\n", total);
        }
        free(plans);
    } else if (!strcmp(command, "insert_song") && arguments == 1) {
        result = insert_song(db, a, &id);
        if (id) printf("Song ID: %lld\n", id);
    } else if (!strcmp(command, "delete_song") && arguments == 1) {
        result = positive_number(a, &number) ? delete_song(db, number) : DB_INVALID_ARGUMENT;
    } else if (!strcmp(command, "scan_songs") && arguments == 0) {
        DIR *folder = opendir(config.audio_directory);
        if (!folder) { perror(config.audio_directory); result = DB_FILE_ERROR; }
        else {
            unsigned registered = 0, skipped = 0, failed = 0;
            struct dirent *entry;
            for (;;) {
                errno = 0; entry = readdir(folder);
                if (!entry) { if (errno) { perror("readdir"); ++failed; } break; }
                size_t length = strlen(entry->d_name);
                if (length <= 4 || strcasecmp(entry->d_name + length - 4, ".mp3")) continue;
                char path[PATH_MAX];
                int written = snprintf(path, sizeof(path), "%s/%s", config.audio_directory, entry->d_name);
                if (written < 0 || (size_t)written >= sizeof(path)) { ++failed; continue; }
                DbResult item = insert_song(db, path, &id);
                printf("%s: %s (song=%lld)\n", db_result_name(item), entry->d_name, id);
                if (item == DB_OK) ++registered;
                else if (item == DB_ALREADY_EXISTS) ++skipped;
                else { fprintf(stderr, "%s\n", db_error(db)); ++failed; }
            }
            closedir(folder);
            printf("Registered/updated=%u, duplicates skipped=%u, failed=%u\n", registered, skipped, failed);
            result = failed ? DB_FILE_ERROR : DB_OK;
        }
    } else { result = DB_INVALID_ARGUMENT; }
    if (result == DB_INVALID_ARGUMENT) {
        db_close(db);
        return command_usage(selected);
    }
    if (result != DB_OK)
        fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db));
    db_close(db);
    return result == DB_OK ? 0 : 1;
}
