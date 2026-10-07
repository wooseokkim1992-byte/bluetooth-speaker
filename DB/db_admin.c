#include "db_api.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name, *usage;
    int arguments, numeric;
} commands[] = {
    {"insert_device", "insert_device UUID BASE|PREMIUM", 2, 0},
    {"delete_device", "delete_device UUID", 1, 0},
    {"update_plan", "update_plan UUID BASE|PREMIUM", 2, 0},
    {"select_member", "select_member UUID", 1, 0},
    {"list_song", "list_song", 0, 0},
    {"list_device", "list_device", 0, 0},
    {"list_plan", "list_plan", 0, 0},
    {"insert_song", "insert_song FILE_PATH", 1, 0},
    {"delete_song", "delete_song SONG_ID", 1, 2}
};

static void usage(void)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        puts(commands[i].usage);
}

static int number(const char *text, uint64_t *out)
{
    if (!text || !*text) return 0;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > UINT64_MAX) return 0;
    *out = (uint64_t)value;
    return 1;
}

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "help")) { usage(); return argc < 2 ? 2 : 0; }
    size_t selected = 0;
    size_t length = sizeof(commands) / sizeof(commands[0]);
    while (selected < length && strcmp(argv[1], commands[selected].name)) ++selected;
    if (selected == length) { usage(); return 2; }
    if (argc - 2 != commands[selected].arguments) { puts(commands[selected].usage); return 2; }
    uint64_t id = 0;
    if (commands[selected].numeric
            && (!number(argv[2], &id) || (commands[selected].numeric == 2 && (!id || id > (uint64_t)LLONG_MAX)))) {
        fprintf(stderr, "Invalid numeric ID\n"); return 2;
    }

    DbConfig config = {
        .host = env_or("DB_HOST", "localhost"),
        .user = env_or("DB_USER", "root"),
        .password = getenv("DB_PASSWORD"),
        .database = env_or("DB_NAME", "speaker_stream")
    };
    Db *db = NULL;
    DbResult result = db_open(&db, &config, NULL);
    if (result != DB_OK) {
        fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db)); db_close(db); return 1;
    }
    switch (selected) {
    case 0: result = insert_device(db, argv[2], argv[3]); break;
    case 1: result = delete_device(db, argv[2]); break;
    case 2: result = update_plan(db, argv[2], argv[3]); break;
    case 3: {
        MemberInfo member;
        result = select_member(db, argv[2], &member);
        if (result == DB_OK) {
            printf("UUID: %s\nMember ID: %llu\nStatus: %s\nPlan: %s (%d bps)\n", member.device.device_uuid,
                (unsigned long long)member.device.member_id, member.device.status,
                member.plan.plan_name, member.plan.bitrate_bps);
            if (member.runtime_available)
                printf("Connected: %d\nPlayback: %.31s\nSong: %lld\nPTS(ms): %llu\n",
                    member.runtime.connected, member.runtime.playback_state,
                    member.runtime.current_song_id, member.runtime.last_played_pts_ms);
            else puts("Live state: unavailable (standalone DB CLI)");
        }
        break;
    }
    case 4: {
        SongInfo *songs = NULL;
        size_t count = 0;
        result = list_song(db, &songs, &count);
        if (result == DB_OK)
            for (size_t i = 0; i < count; ++i)
                printf("%lld | %s | %02lld분 %02lld초\n", songs[i].song_id, songs[i].title,
                    songs[i].duration / 60, songs[i].duration % 60);
        free(songs);
        break;
    }
    case 5: {
        DeviceInfo *devices = NULL;
        size_t count = 0;
        result = list_device(db, &devices, &count);
        if (result == DB_OK)
            for (size_t i = 0; i < count; ++i)
                printf("uuid=%s | member=%llu | %s | %s\n", devices[i].device_uuid,
                    (unsigned long long)devices[i].member_id, devices[i].plan_name, devices[i].status);
        free(devices);
        break;
    }
    case 6: {
        PlanCount *plans = NULL;
        size_t count = 0;
        unsigned long long total = 0;
        result = list_plan(db, &plans, &count, &total);
        if (result == DB_OK) {
            for (size_t i = 0; i < count; ++i)
                printf("%s | members=%llu\n", plans[i].plan_name, plans[i].member_count);
            printf("Total linked members: %llu\n", total);
        }
        free(plans);
        break;
    }
    case 7: {
        long long song_id = 0;
        result = insert_song(db, argv[2], &song_id);
        if (result == DB_OK) printf("Song ID: %lld\n", song_id);
        break;
    }
    case 8: result = delete_song(db, (long long)id); break;
    }
    if (result == DB_OK) puts("OK");
    else fprintf(stderr, "%s: %s\n", db_result_name(result), db_error(db));
    db_close(db);
    return result == DB_OK ? 0 : 1;
}
