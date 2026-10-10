FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential gdb strace ca-certificates \
       libmariadb-dev libcrypt-dev mariadb-client python3 python3-pymysql ffmpeg \
    && rm -rf /var/lib/apt/lists/*

ENV DB_HOST=127.0.0.1 \
    DB_PORT=3306 \
    DB_USER=root \
    DB_PASSWORD=jetson \
    DB_NAME=speaker_stream \
    FILES_DIR=/home/jetson/firmware_project/bluetooth-speaker/files

WORKDIR /app/WEB
COPY Makefile *.c *.h ./
COPY web ./web
RUN make

# Generate the registration program inside the image; no extra source file needed.
RUN printf '%s\n' \
    'import hashlib' \
    'import json' \
    'import os' \
    'from pathlib import Path' \
    'import subprocess' \
    'import pymysql' \
    '' \
    'root = Path(os.environ['\''FILES_DIR'\'']).resolve(strict=True)' \
    'if not root.is_dir():' \
    '    raise RuntimeError(f'\''Not a directory: {root}'\'')' \
    '' \
    'db = pymysql.connect(' \
    '    host=os.environ['\''DB_HOST'\''],' \
    '    port=int(os.environ['\''DB_PORT'\'']),' \
    '    user=os.environ['\''DB_USER'\''],' \
    '    password=os.environ['\''DB_PASSWORD'\''],' \
    '    database=os.environ['\''DB_NAME'\''],' \
    '    charset='\''utf8mb4'\'',' \
    '    autocommit=True,' \
    '    connect_timeout=10,' \
    '    read_timeout=30,' \
    '    write_timeout=30,' \
    ')' \
    '' \
    'inserted = existing = 0' \
    'try:' \
    '    with db.cursor() as cursor:' \
    '        for directory, _, names in os.walk(root, followlinks=False):' \
    '            for name in sorted(names):' \
    '                path = Path(directory) / name' \
    '                if path.is_symlink() or not path.is_file() or path.suffix.lower() != '\''.mp3'\'':' \
    '                    continue' \
    '                path = path.resolve(strict=True)' \
    '                cursor.execute('\''SELECT song_id FROM Song WHERE file_path=%s'\'', (str(path),))' \
    '                if cursor.fetchone() is not None:' \
    '                    existing += 1' \
    '                    print(f'\''EXISTS: {path}'\'', flush=True)' \
    '                    continue' \
    '' \
    '                result = subprocess.run([' \
    '                    '\''ffprobe'\'', '\''-v'\'', '\''error'\'', '\''-select_streams'\'', '\''a:0'\'',' \
    '                    '\''-show_entries'\'', '\''stream=codec_name:format=duration'\'',' \
    '                    '\''-of'\'', '\''json'\'', str(path),' \
    '                ], check=True, capture_output=True, text=True)' \
    '                metadata = json.loads(result.stdout)' \
    '                streams = metadata.get('\''streams'\'', [])' \
    '                if not streams or streams[0].get('\''codec_name'\'') != '\''mp3'\'':' \
    '                    raise RuntimeError(f'\''Not MP3 audio: {path}'\'')' \
    '                duration = float(metadata['\''format'\'']['\''duration'\''])' \
    '                if not 0 < duration < 9223372036854775807:' \
    '                    raise RuntimeError(f'\''Invalid duration: {path}'\'')' \
    '' \
    '                digest = hashlib.sha256()' \
    '                with path.open('\''rb'\'') as audio:' \
    '                    for block in iter(lambda: audio.read(65536), b'\'''\''):' \
    '                        digest.update(block)' \
    '                cursor.execute(' \
    '                    '\''INSERT INTO Song '\''' \
    '                    '\''(title,file_path,codec,duration,file_size_bytes,checksum_sha256) '\''' \
    '                    '\''VALUES (%s,%s,%s,%s,%s,%s) '\''' \
    '                    '\''ON DUPLICATE KEY UPDATE song_id=song_id'\'',' \
    '                    (path.stem[:200], str(path), '\''MP3'\'', int(duration),' \
    '                     path.stat().st_size, digest.hexdigest()),' \
    '                )' \
    '                if cursor.rowcount:' \
    '                    inserted += 1' \
    '                    print(f'\''INSERT: {path}'\'', flush=True)' \
    '                else:' \
    '                    existing += 1' \
    '        print(f'\''Finished: inserted={inserted}, existing={existing}'\'', flush=True)' \
    'finally:' \
    '    db.close()' \
    > /usr/local/bin/import-files.py

RUN printf '%s\n' \
    '#!/bin/sh' \
    'set -eu' \
    'echo "[START] Registering MP3 files from $FILES_DIR"' \
    'python3 /usr/local/bin/import-files.py' \
    'exec "$@"' \
    > /usr/local/bin/speaker-entrypoint \
    && chmod 0755 /usr/local/bin/speaker-entrypoint

ENTRYPOINT ["/usr/local/bin/speaker-entrypoint"]
EXPOSE 8080
CMD ["./web_server", "8080"]
