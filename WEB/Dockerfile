FROM ubuntu:22.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libmariadb-dev libcrypt-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY Makefile *.c *.h ./
RUN make

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libmariadb3 libcrypt1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 webapp

WORKDIR /app
COPY --from=build /build/web_server ./web_server
COPY web/index.html ./web/index.html

ENV DB_HOST=127.0.0.1 \
    DB_NAME=speaker_stream

USER webapp
EXPOSE 8080
CMD ["./web_server", "8080"]
