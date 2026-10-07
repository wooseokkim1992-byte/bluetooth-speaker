# Streaming server build

## 모듈 구성

| 소스 / 헤더 | 역할 | 정적 라이브러리 |
| --- | --- | --- |
| `src/signal_util.c`, `header/signal_util.h` | SIGINT 등록, 종료 요청 플래그 | `build/libsignal_util.a` |
| `src/file_util.c`, `header/file_util.h` | 디렉터리 조회, 쉼표로 구분한 파일 목록 생성 | `build/libfile_util.a` |
| `src/tcp_server.c`, `header/tcp_server.h` | TCP 제어·오디오 생산 스레드, 방송 프레임 송신 | `build/libtcp_server.a` |
| `src/tcp_interface.c`, `header/tcp_interface.h` | 16바이트 헤더와 메시지별 payload 직렬화 | `build/libtcp_interface.a` |
| `server.c` | 인자 처리, 모듈 초기화와 종료 | 실행 파일 진입점 |

기존 `file_util.h`의 송신 함수 선언은 `tcp_server.h`로 이동했다.
signal handler와 종료 플래그, TCP 스레드 함수와 인자 구조체는 모듈 내부로 숨겼다.

## 빌드와 실행

Linux(epoll/eventfd), C11 컴파일러, GNU Make, ar, FFmpeg 실행 파일이 필요하다.
Dockerfile에는 FFmpeg가 포함되어 있다. `files/1.mp3`를 찾을 수 있도록
프로젝트 루트에서 실행한다.

```sh
make
./build/server 9000
```

다른 터미널의 같은 프로젝트 루트에서 테스트 클라이언트를 실행한다.

```sh
./build/client 127.0.0.1 9000
```

서버는 시작할 때 `files/1.mp3`를 44.1 kHz, 16-bit stereo PCM으로
디코딩한다. 오디오 생산 스레드는 파일을 반복 재생하며 20 ms마다
`AUDIO_DATA` 프레임을 만들고, TCP 제어 스레드만 각 PLAYING 클라이언트
소켓에 전송한다. 접속하면 PCM 프로필을 CONNECT_ACK로 알리고
NOW_PLAYING(제목 `1.mp3`)을 보낸다. PAUSE 클라이언트에는 오디오를 보내지
않고 RESUME 이후 최신 방송 시점부터 보낸다. 테스트용 `client.c`는 오디오를
재생하지 않고 수신 바이트 수만 출력한다.

기존 루트의 `server`, `client` 실행 파일은 덮어쓰지 않는다.
새로 빌드한 버전은 반드시 `./build/server`, `./build/client`로 실행한다.

## Makefile 사용법

- `make` / `make all`: 라이브러리, 서버, 클라이언트 빌드.
- `make server`: 서버와 필요한 라이브러리만 빌드.
- `make client`: 기존 테스트 클라이언트만 빌드.
- `make libs`: 정적 라이브러리만 빌드.
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
