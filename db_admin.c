#define _GNU_SOURCE
#include "db_api.h"
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
    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || *end || value <= 0) return 0;
    *out = value;
    return 1;
}

static void usage(const char *name)
{
    printf("Usage: %s [--offline] [--audio-dir PATH] COMMAND [arguments]\n\n", name);
    puts("insert_device UUID plan_id\ndelete_device UUID\nsubscribe_plan UUID\n"
         "cancel_plan UUID\nselect_member UUID\nlist_device\nlist_plan\nlist_song\n"
         "insert_song file_path\ndelete_song song_id\nscan_songs\n\n"
         "--offline: streaming server is stopped (required for standalone deletion).\n"
         "Connection environment: DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, AUDIO_DIR.\n"
         "Default: localhost / root / speaker_stream / /home/jetson/speaker_stream/base");
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
        } else { usage(argv[0]); return 2; }
    }
    if (position == argc) { usage(argv[0]); return 2; }
    const char *command = argv[position++];
    int arguments = argc - position;
    const char *a = arguments > 0 ? argv[position] : NULL;
    const char *b = arguments > 1 ? argv[position + 1] : NULL;
    Db *db = NULL;
    DbResult result = db_open(&db, &config, NULL);
    if (result != DB_OK) {
        fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db)); db_close(db); return 1;
    }
    long long number, id = 0;
    if (!strcmp(command, "insert_device") && arguments == 2) {
        result = positive_number(b, &number) && number <= INT_MAX
            ? insert_device(db, a, (int)number) : DB_INVALID_ARGUMENT;
    } else if (!strcmp(command, "delete_device") && arguments == 1) result = delete_device(db, a);
    else if (!strcmp(command, "subscribe_plan") && arguments == 1) result = subscribe_plan(db, a);
    else if ((!strcmp(command, "cancel_plan") || !strcmp(command, "cancle_plan")) && arguments == 1) result = cancel_plan(db, a);
    else if (!strcmp(command, "select_member") && arguments == 1) {
        MemberInfo member;
        result = select_member(db, a, &member);
        if (result == DB_OK) {
            printf("UUID: %s\nStatus: %s\nPlan: %s (ID %d, %d bps)\n",
                member.device.uuid, member.device.status, member.plan.plan_code,
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
            for (size_t i = 0; i < count; ++i) printf("%s | plan=%d | %s\n", devices[i].uuid, devices[i].plan_id, devices[i].status);
            printf("Total: %zu devices\n", count);
        }
        free(devices);
    } else if (!strcmp(command, "list_song") && arguments == 0) {
        SongInfo *songs = NULL; size_t count = 0;
        result = list_song(db, &songs, &count);
        if (result == DB_OK) {
            for (size_t i = 0; i < count; ++i) {
                printf("%lld | %s | %lld분 %lld초\n", songs[i].song_id, songs[i].title, songs[i].duration / 60, songs[i].duration % 60);
            }
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
    } else { usage(argv[0]); result = DB_INVALID_ARGUMENT; }
    if (result != DB_OK) fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db));
    db_close(db);
    return result == DB_OK ? 0 : 1;
}
