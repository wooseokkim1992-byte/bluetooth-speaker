#!/bin/bash
set -e
cd /home/jetson/speaker_stream
mkdir -p base
shopt -s nullglob

files=(./*.mp3)

if [ ${#files[@]} -eq 0 ]; then
    echo "No have MP3 file"
    exit 1
fi

for f in "${files[@]}"; do
    out="base/$(basename "$f")"

    if [ -f "$out" ]; then
        echo "Already Converted: $out"
        continue
    fi

    echo "Convertion...: $f"
    ffmpeg -nostdin -n -hide_banner -loglevel error \
        -i "$f" -map 0:a:0 -vn \
        -c:a libmp3lame -b:a 128k \
        -ar 44100 -ac 2 "$out"
done

echo "Succes"
ls -lh base/*.mp3
