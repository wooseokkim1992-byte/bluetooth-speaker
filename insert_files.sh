cd ~/firmware_project/bluetooth-speaker/DB

export DB_USER=root
export DB_PASSWORD=jetson
export DB_NAME=speaker_stream

for file in /home/jetson/노래모음/*.mp3; do
    [ -f "$file" ] || continue
    ./db_admin insert_song "$file"
done