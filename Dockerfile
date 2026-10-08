FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        gdb \
        strace \
        ffmpeg \
        ca-certificates \
        mysql-server \
    && apt-get install -y  mariadb-server mariadb-client \
    && apt-get install -y libmariadb-dev \
    && rm -rf /var/lib/apt/lists/

WORKDIR /app

CMD ["/bin/bash", "-c","service mariadb start && mariadb < ./speaker_stream.sql; exec /bin/bash"]
