#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/tcodec-tcmux-mp4-test-$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

command -v ffprobe >/dev/null 2>&1
command -v ffmpeg >/dev/null 2>&1

python3 - "$TMP/input.yuv" <<'PY'
import sys
w, h, n = 32, 24, 3
with open(sys.argv[1], "wb") as f:
    for frame in range(n):
        f.write(bytes((x * 5 + y * 7 + frame * 19) & 255
                      for y in range(h) for x in range(w)))
        f.write(bytes((96 + frame + x + y) & 255
                      for y in range(h // 2) for x in range(w // 2)))
        f.write(bytes((144 + frame + 2 * x + y) & 255
                      for y in range(h // 2) for x in range(w // 2)))
PY

"$ROOT/build/tcenc" -w 32 -h 24 -n 3 -q 32 -p 0 \
    "$TMP/input.yuv" -o "$TMP/input.tcv" >/dev/null 2>&1
"$ROOT/tools/tcmux_mp4.sh" mux -i "$TMP/input.tcv" -o "$TMP/output.mp4" \
    -w 32 -h 24 -f 30

test -s "$TMP/output.mp4"
ffmpeg -nostdin -v error -i "$TMP/output.mp4" -f null -
ffprobe -v error -select_streams v:0 -show_entries format=format_name:stream=codec_name,width,height,nb_frames \
    -of default=nw=1 "$TMP/output.mp4" > "$TMP/probe.txt"
grep -q '^codec_name=h264$' "$TMP/probe.txt"
grep -q '^width=32$' "$TMP/probe.txt"
grep -q '^height=24$' "$TMP/probe.txt"

"$ROOT/tools/tcmux_mp4.sh" demux -i "$TMP/output.mp4" -o "$TMP/roundtrip.yuv"
test "$(wc -c < "$TMP/roundtrip.yuv")" -eq $((3 * 32 * 24 * 3 / 2))

"$ROOT/tools/tcmux_mp4.sh" segment -i "$TMP/input.tcv" -o "$TMP/hls" \
    -w 32 -h 24 -f 30 -s 1 -p "$TMP/playlists/playlist.m3u8"
test -s "$TMP/hls/playlist.m3u8"
test -s "$TMP/hls/init.mp4"
test -s "$TMP/hls/seg_0000.m4s"
test -s "$TMP/playlists/playlist.m3u8"
# Every playlist URI must resolve relative to the generated segment directory.
python3 - "$TMP/hls/playlist.m3u8" "$TMP/hls" <<'PY'
import pathlib, sys
playlist, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for line in playlist.read_text().splitlines():
    if line and not line.startswith('#'):
        assert (root / line).is_file(), line
PY
ffmpeg -nostdin -v error -i "$TMP/hls/playlist.m3u8" -f null -

# Invalid dimensions and missing inputs must fail without producing a result.
if "$ROOT/tools/tcmux_mp4.sh" mux -i "$TMP/input.tcv" -o "$TMP/bad.mp4" -w 0 -h 24 -f 30; then
    echo "tcmux_mp4: accepted zero width" >&2
    exit 1
fi
printf 'preserve-me' > "$TMP/preserved.yuv"
if "$ROOT/tools/tcmux_mp4.sh" demux -i "$TMP/missing.mp4" -o "$TMP/preserved.yuv"; then
    echo "tcmux_mp4: accepted missing input" >&2
    exit 1
fi
[ "$(cat "$TMP/preserved.yuv")" = preserve-me ]

if "$ROOT/tools/tcmux_mp4.sh" demux -i "$TMP/missing.mp4" -o "$TMP/missing.yuv"; then
    echo "tcmux_mp4: accepted missing input" >&2
    exit 1
fi

printf '%s\n' 'tcmux_mp4: playable MP4 bridge OK'
