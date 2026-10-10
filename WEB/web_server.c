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
    if (db_query(db, "SELECT 1 FROM Device WHERE member_id=%llu LIMIT 1",
            (unsigned long long)user->member_id) != DB_OK) {
        fprintf(stderr, "Device lookup failed: %s\n", db_error(db));
        message(fd, 503, "기기 등록 상태를 조회할 수 없습니다."); return;
    }
    MYSQL_RES *rows = NULL;
    if (db_get_result(db, &rows) != DB_OK) {
        message(fd, 503, "기기 등록 상태를 조회할 수 없습니다."); return;
    }
    int registered = mysql_num_rows(rows) != 0;
    mysql_free_result(rows);
    char json[256];
    /* Strings preserve all 64 bits in JavaScript. */
    snprintf(json, sizeof(json),
        "{\"message\":\"로그인되었습니다.\",\"member_id\":\"%llu\",\"device_registered\":%s}",
        (unsigned long long)user->member_id, registered ? "true" : "false");
    web_reply(fd, 200, "application/json; charset=utf-8", json, cookie);
}

static void register_device(int fd, Db *db, const WebSession *user, const char *uuid)
{
    size_t length = strlen(uuid);
    if (!length || length > 36) {
        message(fd, 400, "device_uuid는 1~36바이트로 입력해주세요."); return;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)uuid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-' || c == '_')) {
            message(fd, 400, "UUID는 영문, 숫자, 하이픈, 밑줄만 사용할 수 있습니다."); return;
        }
    }
    char *value = db_text_value(uuid);
    if (!value) { message(fd, 503, "메모리가 부족합니다."); return; }
    DbResult result = db_query(db,
        "INSERT INTO Device(device_uuid,member_id,plan_name,status) "
        "VALUES(%s,%llu,'BASE','ACTIVE')", value,
        (unsigned long long)user->member_id);
    if (result == DB_OK) {
        free(value);
        message(fd, 201, "기기 등록이 완료되었습니다."); return;
    }
    unsigned int error = mysql_errno(db->connection);
    if (error == 1062) {
        // 같은 회원의 재전송은 성공 처리. 다른 회원/미소유 기기를 가져오지 않는다.
        result = db_query(db, "SELECT member_id FROM Device WHERE device_uuid=%s", value);
        free(value);
        MYSQL_RES *rows = NULL;
        if (result != DB_OK || db_get_result(db, &rows) != DB_OK) {
            message(fd, 503, "기기 등록 상태를 확인할 수 없습니다."); return;
        }
        MYSQL_ROW row = mysql_fetch_row(rows);
        int same_member = row && row[0] &&
            strtoull(row[0], NULL, 10) == user->member_id;
        mysql_free_result(rows);
        if (same_member) message(fd, 200, "이미 본인 계정에 등록된 기기입니다.");
        else message(fd, 409, "이미 등록된 UUID입니다. 다른 UUID를 입력해주세요.");
        return;
    }
    free(value);
    fprintf(stderr, "Device registration failed: %s\n", db_error(db));
    if (error == 1452) message(fd, 503, "회원 정보와 기본 요금제 BASE를 확인해주세요.");
    else message(fd, 500, "기기 등록에 실패했습니다. 서버 터미널을 확인해주세요.");
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
    int is_device = !strcmp(request->path, "/api/register-device");
    if (!is_signup && !is_login && !is_device) { message(fd, 404, "API를 찾을 수 없습니다."); return; }
    WebSession owner = {0};
    if (is_device) {
        char token[65];
        if (!web_cookie_token(request->cookie, token) || !web_session_find(token, &owner)) {
            message(fd, 401, "로그인 후 기기를 등록해주세요."); return;
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
        register_device(fd, db, &owner, form.device_uuid);
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
