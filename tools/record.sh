#!/bin/sh
# Record a clip with sound:  tools/record.sh out.mp4 [mechvr options: --len --lookat --orbit --pilot ...]
# Frames go from the game straight into ffmpeg through a pipe. SIZE and FPS can be set in the environment
# (defaults 1280x720 and 30; FPS must divide 90). The result is near-lossless; tools/fit.sh shrinks it for sharing.
set -e
out=$1; shift
W=${SIZE%x*}; H=${SIZE#*x}; W=${W:-1280}; H=${H:-720}; FPS=${FPS:-30}
[ -n "$SIZE" ] || { W=1280; H=720; }
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkfifo "$tmp/frames"
ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s "${W}x${H}" -r "$FPS" -i "$tmp/frames" \
       -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p "$tmp/video.mp4" &
encoder=$!
"$here/mechvr" --video "$tmp/frames" --wav "$tmp/sound.wav" --size "$W" "$H" --fps "$FPS" "$@" >"$tmp/log" 2>&1 || true
if ! grep -q recorded "$tmp/log"; then # the game never opened the pipe, so the encoder would wait forever
    kill "$encoder" 2>/dev/null || true
    cat "$tmp/log"
    exit 1
fi
wait
grep -E "recorded|report" "$tmp/log" || true
ffmpeg -loglevel error -y -i "$tmp/video.mp4" -i "$tmp/sound.wav" -c:v copy -c:a aac -b:a 160k -shortest "$out"
