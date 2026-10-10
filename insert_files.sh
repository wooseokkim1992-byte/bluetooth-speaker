#!/usr/bin/env bash
set -euo pipefail

# Run inside the container that will stream Song.file_path.
# Usage: bash insert_files.sh [--dry-run] [mariadb client options]
# Example: bash insert_files.sh --user=root --password

project_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
files_dir="$project_dir/files"
dry_run=false
if [[ ${1:-} == --dry-run ]]; then
    dry_run=true
    shift
fi

if [[ ! -d $files_dir ]]; then
    printf 'files 디렉터리를 찾을 수 없습니다: %s\n' "$files_dir" >&2
    exit 1
fi
files_dir=$(realpath -- "$files_dir")

for required in ffprobe sha256sum od tr find sort awk wc cut; do
    if ! command -v "$required" >/dev/null 2>&1; then
        printf '필요한 명령이 없습니다: %s\n' "$required" >&2
        exit 1
    fi
done
if [[ $dry_run == false ]] && ! command -v mariadb >/dev/null 2>&1; then
    printf 'MariaDB 클라이언트(mariadb)가 필요합니다.\n' >&2
    exit 1
fi

hex_utf8() {
    printf '%s' "$1" | od -An -tx1 -v | tr -d '[:space:]'
}

emit_sql() {
    local file title codec duration_raw duration_seconds file_size checksum
    local title_hex path_hex codec_hex probe
    local inserted=0 skipped=0

    printf 'SET NAMES utf8mb4;\nSTART TRANSACTION;\n'
    while IFS= read -r -d '' file; do
        title=${file##*/}
        [[ $title == *.* ]] && title=${title%.*}
        if [[ -z $title ]]; then
            printf '건너뜀(제목 없음): %s\n' "$file" >&2
            ((skipped += 1))
            continue
        fi

        if ! probe=$(ffprobe -v error -select_streams a:0 \
            -show_entries stream=codec_name:format=duration \
            -of default=noprint_wrappers=1 -i "$file" 2>/dev/null); then
            printf '건너뜀(메타데이터 읽기 실패): %s\n' "$file" >&2
            ((skipped += 1))
            continue
        fi
        codec=$(awk -F= '$1 == "codec_name" { print $2; exit }' <<< "$probe")
        duration_raw=$(awk -F= '$1 == "duration" { print $2; exit }' <<< "$probe")
        codec=$(printf '%s' "$codec" | tr '[:lower:]' '[:upper:]')
        if [[ -z $codec || ${#codec} -gt 16 ]] ||
            ! duration_seconds=$(awk -v value="$duration_raw" 'BEGIN {
                if (value !~ /^[0-9]+([.][0-9]+)?$/ || value + 0 <= 0) exit 1
                printf "%.0f", int(value)
            }'); then
            printf '건너뜀(오디오 코덱 또는 재생 시간 오류): %s\n' "$file" >&2
            ((skipped += 1))
            continue
        fi

        if ! file_size=$(wc -c < "$file") ||
            ! checksum=$(sha256sum --zero -- "$file" | cut -c1-64); then
            printf '건너뜀(파일 크기 또는 체크섬 계산 실패): %s\n' "$file" >&2
            ((skipped += 1))
            continue
        fi
        file_size=${file_size//[[:space:]]/}
        if [[ ! $checksum =~ ^[0-9a-f]{64}$ ]]; then
            printf '건너뜀(체크섬 형식 오류): %s\n' "$file" >&2
            ((skipped += 1))
            continue
        fi

        title_hex=$(hex_utf8 "$title")
        path_hex=$(hex_utf8 "$file")
        codec_hex=$(hex_utf8 "$codec")
        printf 'INSERT INTO Song (title, file_path, codec, duration, file_size_bytes, checksum_sha256) VALUES (CONVERT(0x%s USING utf8mb4), CONVERT(0x%s USING utf8mb4), CONVERT(0x%s USING utf8mb4), %s, %s, '\''%s'\'') ON DUPLICATE KEY UPDATE title=VALUES(title), codec=VALUES(codec), duration=VALUES(duration), file_size_bytes=VALUES(file_size_bytes), checksum_sha256=VALUES(checksum_sha256);\n' \
            "$title_hex" "$path_hex" "$codec_hex" "$duration_seconds" "$file_size" "$checksum"
        printf '등록 준비: %s\n' "$file" >&2
        ((inserted += 1))
    done < <(find "$files_dir" -type f -print0 | sort -z)

    if ((inserted == 0)); then
        printf '등록 가능한 오디오 파일이 없습니다.\n' >&2
        return 1
    fi
    printf 'COMMIT;\n'
    printf '등록 대상 %d개, 건너뜀 %d개\n' "$inserted" "$skipped" >&2
}

if [[ $dry_run == true ]]; then
    emit_sql
else
    emit_sql | mariadb "$@" --database=speaker_stream
fi
