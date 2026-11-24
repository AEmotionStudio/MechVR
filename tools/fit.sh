#!/bin/sh
# Shrink a clip to fit a size limit (default 9.5 MB, under Discord's 10 MB for free accounts):
#   tools/fit.sh in.mp4 out.mp4 [megabytes]
# Two-pass H.264 at exactly the bit rate that fits, with 128 kbit/s sound.
set -e
in=$1; out=$2; mb=${3:-9.5}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
len=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$in")
kbps=$(python3 -c "print(int($mb * 8 * 1000 * 0.97 / $len - 128))")
cd "$tmp"
ffmpeg -loglevel error -y -i "$in" -c:v libx264 -preset slow -b:v "${kbps}k" -pass 1 -an -f mp4 /dev/null
ffmpeg -loglevel error -y -i "$in" -c:v libx264 -preset slow -b:v "${kbps}k" -pass 2 -c:a aac -b:a 128k -movflags +faststart "$out"
echo "$out: $(stat -c %s "$out") bytes, ${len%.*} s, video ${kbps} kbit/s"
