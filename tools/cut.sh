#!/bin/sh
# Join pieces of recorded clips, with short sound fades at each cut and a fade in and out overall:
#   tools/cut.sh out.mp4 clip1.mp4 start end [clip2.mp4 start end ...]
# Times are in seconds. The result is near-lossless; tools/fit.sh shrinks it for sharing.
set -e
out=$1; shift
inputs=""; filters=""; joins=""; n=0; total=0
while [ $# -ge 3 ]; do
    inputs="$inputs -i $1"
    len=$(python3 -c "print($3 - $2)")
    filters="$filters[$n:v]trim=$2:$3,setpts=PTS-STARTPTS[v$n];[$n:a]atrim=$2:$3,asetpts=PTS-STARTPTS,afade=t=in:d=0.04,afade=t=out:st=$(python3 -c "print(max($len - 0.04, 0))"):d=0.04[a$n];"
    joins="$joins[v$n][a$n]"
    total=$(python3 -c "print($total + $len)")
    n=$((n + 1)); shift 3
done
end=$(python3 -c "print(max($total - 0.6, 0))")
filters="$filters${joins}concat=n=$n:v=1:a=1[cv][ca];[cv]fade=t=in:d=0.5,fade=t=out:st=$end:d=0.6[v];[ca]afade=t=in:d=0.5,afade=t=out:st=$end:d=0.6[a]"
# shellcheck disable=SC2086
ffmpeg -loglevel error -y $inputs -filter_complex "$filters" -map "[v]" -map "[a]" -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p -c:a aac -b:a 192k "$out"
echo "$out: ${total} s from $n pieces"
