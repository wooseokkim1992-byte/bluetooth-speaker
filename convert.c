#include <stdio.h>
#include <mysql.h>

int main(void)
{
    MYSQL *db = mysql_init(NULL);
    if (db == NULL) {
        fprintf(stderr, "DB Init failed\n");
        return 1;
    }

    if (mysql_real_connect(db, "localhost", "root", NULL,
                           "speaker_stream", 0, NULL, 0) == NULL) {
        fprintf(stderr, "DB Connecting failed: %s\n", mysql_error(db));
        mysql_close(db);
        return 1;
    }

    const char *sql =
        "SELECT song_id, title, file_path FROM Song ORDER BY song_id";

    if (mysql_set_character_set(db, "utf8mb4") != 0 ||
        mysql_query(db, sql) != 0) {
        fprintf(stderr, "DB Search failed: %s\n", mysql_error(db));
        mysql_close(db);
        return 1;
    }

    MYSQL_RES *result = mysql_store_result(db);
    if (result == NULL) {
        fprintf(stderr, "Read Result Failed: %s\n", mysql_error(db));
        mysql_close(db);
        return 1;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != NULL) {
        printf("[%s] %s\n", row[0], row[1]);

        FILE *file = fopen(row[2], "rb");
        if (file == NULL) {
            perror(row[2]);
        } else {
            printf("  File Open Succes: %s\n", row[2]);
            fclose(file);
        }
    }

    mysql_free_result(result);
    mysql_close(db);
    return 0;
}
