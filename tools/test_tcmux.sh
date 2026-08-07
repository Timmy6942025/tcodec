#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/tcodec-tcmux-test-$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/segments"

python3 - "$TMP/input.yuv" <<'PY'
import sys
w, h, n = 16, 16, 3
with open(sys.argv[1], "wb") as f:
    for frame in range(n):
        f.write(bytes((x * 9 + y * 13 + frame * 17) & 255
                      for y in range(h) for x in range(w)))
        f.write(bytes((96 + frame + x + y) & 255
                      for y in range(h // 2) for x in range(w // 2)))
        f.write(bytes((144 + frame + 2 * x + y) & 255
                      for y in range(h // 2) for x in range(w // 2)))
PY

"$ROOT/build/tcenc" -w 16 -h 16 -n 3 -q 32 -p 0 \
    "$TMP/input.yuv" -o "$TMP/input.tcv" >/dev/null 2>&1
"$ROOT/build/tcmux" mux -i "$TMP/input.tcv" -o "$TMP/stream.tcmx" -f 30
"$ROOT/build/tcmux" demux -i "$TMP/stream.tcmx" -o "$TMP/demux.tcv"
cmp "$TMP/input.tcv" "$TMP/demux.tcv"
# Native TCV-in-ISO-BMFF carriage must round-trip byte-for-byte. The private
# tcv1 sample entry is not expected to be playable by stock FFmpeg.
"$ROOT/build/tcmux" mp4mux -i "$TMP/input.tcv" -o "$TMP/native.mp4" -f 30
"$ROOT/build/tcmux" mp4demux -i "$TMP/native.mp4" -o "$TMP/native-demux.tcv"
cmp "$TMP/input.tcv" "$TMP/native-demux.tcv"
python3 - "$TMP/native.mp4" <<'PY'
import pathlib
p = pathlib.Path(__import__('sys').argv[1]).read_bytes()
assert b"ftyp" in p and b"moov" in p and b"mdat" in p and b"tcv1" in p
PY
# Native tcv1 is intentionally private: stock FFmpeg must not decode it as
# AVC/H.264. ffprobe may still parse the ISO-BMFF metadata, so exercise the
# decoder path rather than requiring metadata probing to fail.
if ffmpeg -nostdin -v error -i "$TMP/native.mp4" -f null - >/dev/null 2>&1; then
    echo "tcmux: stock ffmpeg unexpectedly decoded private tcv1" >&2
    exit 1
fi
"$ROOT/build/tcmux" segment -i "$TMP/stream.tcmx" -o "$TMP/segments" -s 2 \
    -p "$TMP/playlist.m3u8"
test -s "$TMP/playlist.m3u8"
test -s "$TMP/segments/seg_0000.m4s"
# A segment stream that starts on a non-keyframe must be rejected.
python3 - "$TMP/nonkey.tcmx" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"TCMX" + struct.pack("<III", 1, 30, 0))
    f.write(b"PKT0" + struct.pack("<QIB3x", 0, 9, 0) + b"TCV" + bytes(6))
PY
if "$ROOT/build/tcmux" segment -i "$TMP/nonkey.tcmx" -o "$TMP/badsegments" -s 2; then
    echo "tcmux: accepted non-keyframe segment start" >&2
    exit 1
fi
# Every segment must carry a TCMF header and begin with a keyframe record.
python3 - "$TMP/segments" <<'PY'
import glob, struct, sys
for path in sorted(glob.glob(sys.argv[1] + "/seg_*.m4s")):
    with open(path, "rb") as f:
        assert f.read(4) == b"TCMF"
        f.read(12)
        assert f.read(4) == b"PKT0"
        record = f.read(16)
        assert len(record) == 16
        _, size, key = struct.unpack("<QIB3x", record)
        assert key == 1, path
        assert len(f.read(size)) == size
PY
# Truncated and malformed containers must fail cleanly.
printf 'TCMX' > "$TMP/truncated.tcmx"
if "$ROOT/build/tcmux" demux -i "$TMP/truncated.tcmx" -o "$TMP/truncated.tcv"; then
    echo "tcmux: accepted truncated header" >&2
    exit 1
fi
# Partial raw lengths, invalid numeric arguments, bad versions, and bad records.
printf '\001\002' > "$TMP/rawshort"
if "$ROOT/build/tcmux" mux -i "$TMP/rawshort" -o "$TMP/rawshort.tcmx"; then exit 1; fi
if "$ROOT/build/tcmux" mux -i "$TMP/input.tcv" -o "$TMP/badfps.tcmx" -f 0; then exit 1; fi
python3 - "$TMP/badversion.tcmx" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"TCMX" + struct.pack("<III", 99, 30, 0))
PY
if "$ROOT/build/tcmux" demux -i "$TMP/badversion.tcmx" -o "$TMP/badversion.tcv"; then exit 1; fi
python3 - "$TMP/badrecord.tcmx" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"TCMX" + struct.pack("<III", 1, 30, 0))
    f.write(b"BAD0" + struct.pack("<QIB3x", 0, 14, 1) + b"TCV" + bytes(11))
PY
if "$ROOT/build/tcmux" demux -i "$TMP/badrecord.tcmx" -o "$TMP/badrecord.tcv"; then exit 1; fi
python3 - "$TMP/badpayload.tcmx" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"TCMX" + struct.pack("<III", 1, 30, 0))
    f.write(b"PKT0" + struct.pack("<QIB3x", 0, 14, 1) + b"TCV" + bytes(4))
PY
if "$ROOT/build/tcmux" demux -i "$TMP/badpayload.tcmx" -o "$TMP/badpayload.tcv"; then exit 1; fi
python3 - "$TMP/badflag.tcmx" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"TCMX" + struct.pack("<III", 1, 30, 0))
    f.write(b"PKT0" + struct.pack("<QIB3x", 0, 14, 2) + b"TCV" + bytes(11))
PY
if "$ROOT/build/tcmux" demux -i "$TMP/badflag.tcmx" -o "$TMP/badflag.tcv"; then exit 1; fi
printf '%s\n' 'tcmux: mux/demux/segment round-trip OK'
