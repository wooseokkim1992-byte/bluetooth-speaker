#define _GNU_SOURCE
#include "web_http.h"
#include "web_session.h"
#include "signup.h"
#include "login.h"
#include "db_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_server(int signal_number) { (void)signal_number; stopping = 1; }
static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}
static void message(int fd, int status, const char *text)
{
    /* Only fixed server strings are supplied here, never user input. */
    char json[512];
    snprintf(json, sizeof(json), "{\"message\":\"%s\"}", text);
    web_reply(fd, status, "application/json; charset=utf-8", json, NULL);
}

static void show_page(int fd)
{
    FILE *file = fopen("web/index.html", "rb");
    if (!file) { message(fd, 500, "web/index.html 파일을 찾을 수 없습니다."); return; }
    char page[32768];
    size_t length = fread(page, 1, sizeof(page) - 1, file);
    int failed = ferror(file) || (length == sizeof(page) - 1 && fgetc(file) != EOF);
    fclose(file);
    if (failed) { message(fd, 500, "화면 파일을 읽을 수 없습니다."); return; }
    page[length] = '\0';
    web_reply(fd, 200, "text/html; charset=utf-8", page, NULL);
}

static DbResult open_web_db(Db **db)
{
    DbConfig config = {
        .host = env_or("DB_HOST", "localhost"), .user = env_or("DB_USER", "root"),
        .password = getenv("DB_PASSWORD"), .database = env_or("DB_NAME", "speaker_stream"),
        .port = 3306
    };
    return db_open(db, &config, NULL);
}

static void session_reply(int fd, Db *db, const WebSession *user, const char *cookie)
{
    (void)db;
    char json[256];
    /* Every login asks for the client ID; a previously linked device is not proof. */
    snprintf(json, sizeof(json),
        "{\"message\":\"로그인되었습니다. 기기 ID를 확인해주세요.\",\"member_id\":\"%llu\",\"device_registered\":false}",
        (unsigned long long)user->member_id);
    web_reply(fd, 200, "application/json; charset=utf-8", json, cookie);
}

/* Decimal browser input represents uint64_t, never a JavaScript Number. */
static int parse_client_id(const char *text, uint64_t *out)
{
    if (!text || !*text || strlen(text) > 20) return -1;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        uint64_t digit = *p - '0';
        if (value > (UINT64_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

static void verify_device(int fd, Db *db, const WebSession *user, const char *text)
{
    uint64_t client_id;
    if (parse_client_id(text, &client_id)) {
        message(fd, 400, "client_id는 0~18446744073709551615 사이의 십진수로 입력해주세요.");
        return;
    }
    char decimal[21];
    snprintf(decimal, sizeof(decimal), "%llu", (unsigned long long)client_id);
    char *value = db_text_value(decimal);
    if (!value) { message(fd, 503, "메모리가 부족합니다."); return; }

    /* Only an existing, unowned device may be linked. Never create a Device here.
     * The conditional UPDATE prevents concurrent claims from replacing an owner.
     * device_uuid retains its existing name and stores canonical decimal text.
     */
    DbResult status = db_query(db,
        "UPDATE Device SET member_id=%llu WHERE device_uuid=%s AND member_id IS NULL",
        (unsigned long long)user->member_id, value);
    if (status == DB_OK)
        status = db_query(db, "SELECT member_id FROM Device WHERE device_uuid=%s", value);
    free(value);
    MYSQL_RES *rows = NULL;
    if (status != DB_OK || db_get_result(db, &rows) != DB_OK) {
        fprintf(stderr, "Device verification failed: %s\n", db_error(db));
        message(fd, 503, "기기 ID를 확인할 수 없습니다."); return;
    }
    MYSQL_ROW row = mysql_fetch_row(rows);
    int found = row != NULL;
    int owned = row && row[0] && strtoull(row[0], NULL, 10) == user->member_id;
    mysql_free_result(rows);
    if (!found) message(fd, 404, "DB에 등록되지 않은 기기 ID입니다.");
    else if (!owned) message(fd, 409, "다른 회원에게 연결된 기기입니다.");
    else message(fd, 200, "기기 ID가 일치합니다. 회원 연결이 확인되었습니다.");
}

static void handle_request(int fd, WebRequest *request)
{
    if (!strcmp(request->method, "GET")) {
        if (!strcmp(request->path, "/")) { show_page(fd); return; }
        if (!strcmp(request->path, "/favicon.ico")) {
            web_reply(fd, 204, "image/x-icon", "", NULL); return;
        }
        if (!strcmp(request->path, "/api/session")) {
            char token[65];
            WebSession user;
            if (web_cookie_token(request->cookie, token) && web_session_find(token, &user)) {
                Db *db = NULL;
                if (open_web_db(&db) == DB_OK) session_reply(fd, db, &user, NULL);
                else message(fd, 503, "DB에 연결할 수 없습니다.");
                db_close(db);
            } else message(fd, 401, "로그인이 필요합니다.");
            return;
        }
        message(fd, 404, "페이지를 찾을 수 없습니다."); return;
    }
    if (strcmp(request->method, "POST")) { message(fd, 405, "지원하지 않는 요청입니다."); return; }
    if (!web_same_origin(request)) { message(fd, 403, "같은 웹 페이지에서 요청해주세요."); return; }
    if (!strcmp(request->path, "/api/logout")) {
        char token[65];
        if (web_cookie_token(request->cookie, token)) web_session_remove(token);
        web_reply(fd, 200, "application/json; charset=utf-8", "{\"message\":\"로그아웃되었습니다.\"}",
            "speaker_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        return;
    }
    int is_signup = !strcmp(request->path, "/api/signup");
    int is_login = !strcmp(request->path, "/api/login");
    int is_device = !strcmp(request->path, "/api/verify-device") ||
                    !strcmp(request->path, "/api/register-device");
    if (!is_signup && !is_login && !is_device) { message(fd, 404, "API를 찾을 수 없습니다."); return; }
    WebSession owner = {0};
    if (is_device) {
        char token[65];
        if (!web_cookie_token(request->cookie, token) || !web_session_find(token, &owner)) {
            message(fd, 401, "로그인 후 기기 ID를 확인해주세요."); return;
        }
    }
    WebForm form = {0};
    if (web_parse_form(request, &form)) {
        explicit_bzero(&form, sizeof(form));
        message(fd, 400, "입력 형식을 확인해주세요."); return;
    }
    if (is_signup && strcmp(form.password, form.confirm)) {
        explicit_bzero(&form, sizeof(form));
        message(fd, 400, "비밀번호 확인이 일치하지 않습니다."); return;
    }
    Db *db = NULL;
    if (open_web_db(&db) != DB_OK) {
        explicit_bzero(&form, sizeof(form));
        fprintf(stderr, "Database connection failed: %s\n", db_error(db));
        db_close(db); message(fd, 503, "DB에 연결할 수 없습니다."); return;
    }
    if (is_device) {
        verify_device(fd, db, &owner, form.client_id);
        explicit_bzero(&form, sizeof(form));
        db_close(db);
    } else if (is_signup) {
        SignupInfo info;
        SignupResult result = signup_member(db,
                                            form.login_id, form.password, &info);
        explicit_bzero(&form, sizeof(form));
        db_close(db);
        if (result == SIGNUP_OK) message(fd, 201, "회원가입이 완료되었습니다. 로그인해주세요.");
        else if (result == SIGNUP_ALREADY_EXISTS) message(fd, 409, "이미 등록된 ID입니다.");
        else if (result == SIGNUP_INVALID_INPUT) message(fd, 400, "ID 형식 또는 비밀번호 길이를 확인해주세요.");
        else message(fd, 500, "회원가입 처리에 실패했습니다.");
    } else {
        LoginInfo info;
        LoginResult result = login_member(db, form.login_id, form.password, &info);
        explicit_bzero(&form, sizeof(form));
        if (result == LOGIN_INVALID) { db_close(db); message(fd, 401, "ID 또는 비밀번호를 확인해주세요."); return; }
        if (result != LOGIN_OK) { db_close(db); message(fd, 500, "로그인 처리에 실패했습니다."); return; }
        char token[65], cookie[192];
        if (web_session_create(info.member_id, token)) {
            db_close(db);
            message(fd, 503, "로그인 세션을 만들 수 없습니다. 잠시 후 다시 시도해주세요."); return;
        }
        /* Replace this browser's old session after successful authentication. */
        char previous[65];
        if (web_cookie_token(request->cookie, previous)) web_session_remove(previous);
        snprintf(cookie, sizeof(cookie),
            "speaker_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600", token);
        WebSession user = {info.member_id};
        session_reply(fd, db, &user, cookie);
        db_close(db);
        explicit_bzero(token, sizeof(token));
    }
}

int main(int argc, char **argv)
{
    char *end;
    const char *port_text = argc == 2 ? argv[1] : "8080";
    errno = 0;
    long port = strtol(port_text, &end, 10);
    if (argc > 2 || errno || *end || port < 1 || port > 65535) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]); return 2;
    }
    struct sigaction action = {0};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 16)) {
        perror("listen/bind"); close(listener); return 1;
    }
    printf("Speaker web server: http://<Jetson-IP>:%ld (Ctrl+C to stop)\n", port);
    fflush(stdout);
    while (!stopping) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        struct timeval timeout = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        WebRequest request;
        int error = web_read_request(fd, &request);
        if (error) message(fd, error, "요청을 읽을 수 없습니다.");
        else handle_request(fd, &request);
        explicit_bzero(&request, sizeof(request));
        close(fd);
    }
    close(listener);
    return 0;
}
