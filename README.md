# Streaming server build

## 모듈 구성

| 소스 / 헤더 | 역할 | 정적 라이브러리 |
| --- | --- | --- |
| `src/signal_util.c`, `header/signal_util.h` | SIGINT 등록, 종료 요청 플래그 | `build/libsignal_util.a` |
| `src/file_util.c`, `header/file_util.h` | 디렉터리 조회, 쉼표로 구분한 파일 목록 생성 | `build/libfile_util.a` |
| `src/tcp_server.c`, `header/tcp_server.h` | TCP listen/accept, 클라이언트 스레드, 길이·목록 송신 | `build/libtcp_server.a` |
| `server.c` | 인자 처리, 모듈 초기화와 종료 | 실행 파일 진입점 |

기존 `file_util.h`의 송신 함수 선언은 `tcp_server.h`로 이동했다.
signal handler와 종료 플래그, TCP 스레드 함수와 인자 구조체는 모듈 내부로 숨겼다.

## 빌드와 실행

POSIX C 컴파일러, GNU Make, ar가 필요하다. 프로젝트 루트에서 실행한다.

```sh
make
./build/server 9000
```

다른 터미널의 같은 프로젝트 루트에서 테스트 클라이언트를 실행한다.

```sh
./build/client 127.0.0.1 9000
```

서버는 기존처럼 실행 디렉터리 기준 `./files`를 읽는다.
프로토콜도 동일하다: 4바이트 network byte order 길이와 해당 길이만큼의
쉼표 구분 파일명 문자열을 전송한다. 문자열 종료 NUL은 보내지 않는다.
목록 전송 후 연결을 닫는 동작과 SIGINT 기반 종료 방식은 유지했다.
A2DP나 새로운 재생 로직은 추가하지 않았다.

기존 루트의 `server`, `client` 실행 파일은 덮어쓰지 않는다.
새로 빌드한 버전은 반드시 `./build/server`, `./build/client`로 실행한다.

## Makefile 사용법

- `make` / `make all`: 라이브러리, 서버, 클라이언트 빌드.
- `make server`: 서버와 필요한 라이브러리만 빌드.
- `make client`: 기존 테스트 클라이언트만 빌드.
- `make libs`: 정적 라이브러리 3개만 빌드.
- `make -j4`: 병렬 빌드.
- `make clean`: 이 Makefile에서 생성한 파일만 제거. 소스·음원·기존 루트 실행 파일은 유지.
- `make CC=gcc`: 컴파일러 선택. `AR`, `CFLAGS`, `CPPFLAGS`,
  `LDFLAGS`, `LDLIBS`도 명령행에서 지정할 수 있다.
  `CPPFLAGS`를 덮어쓸 때는 `-Iheader -D_POSIX_C_SOURCE=200809L`을 포함한다.

각 .c를 .o로 컴파일하고, `ar rcs`로 모듈마다 .a를 만든 뒤 서버에 링크한다.
TCP 모듈은 파일·signal 모듈에 의존하므로 링크 명령에서 TCP 라이브러리가 먼저 온다.
`-MMD -MP`로 헤더 의존성을 기록하여 헤더 변경 시 관련 모듈도 다시 빌드한다.
클라이언트는 서버 라이브러리를 사용하지 않는다.

컴파일러·플래그 변경은 자동 감지하지 않으므로 이를 바꾸기 전에는 `make clean`을 실행한다.
