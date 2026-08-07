#!/bin/sh
# soak_1080p.sh — bounded generated-content Tier-1 correctness soak.
#
# This deliberately keeps the large input and decoded output outside the
# repository. It validates that every requested frame can be encoded and
# decoded at 1920x1080, while reporting throughput honestly.
#
# Usage: make soak-1080p
# Environment: TCODEC_SOAK_FRAMES, TCODEC_SOAK_QP,
#              TCODEC_SOAK_THREADS, TCODEC_SOAK_OUT
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
FRAMES=${TCODEC_SOAK_FRAMES:-300}
QP=${TCODEC_SOAK_QP:-42}
# tcenc accepts -t; tcdec currently has no thread option, so this controls
# encoder threads only. Decode timing below is intentionally serial.
THREADS=${TCODEC_SOAK_THREADS:-1}
OUT=${TCODEC_SOAK_OUT:-/tmp/tcodec-soak}
W=1920
H=1080
FRAME_BYTES=$((W * H * 3 / 2))

case "$FRAMES" in ''|*[!0-9]*|0) echo "soak: frames must be positive" >&2; exit 2;; esac
case "$QP" in ''|*[!0-9]*) echo "soak: qp must be numeric" >&2; exit 2;; esac
case "$THREADS" in ''|*[!0-9]*|0) echo "soak: threads must be positive" >&2; exit 2;; esac

command -v ffmpeg >/dev/null 2>&1 || {
    echo "soak: ffmpeg is required to generate the input" >&2
    exit 1
}

mkdir -p "$OUT"
SRC="$OUT/input_${W}x${H}_${FRAMES}f.yuv"
BIT="$OUT/output_qp${QP}.tcv"
DEC="$OUT/decoded.yuv"
ENC_LOG="$OUT/encode.log"
DEC_LOG="$OUT/decode.log"

if [ ! -s "$SRC" ]; then
    echo "soak: generating $FRAMES frames at ${W}x${H}"
    ffmpeg -nostdin -v error -y \
        -f lavfi -i "testsrc2=size=${W}x${H}:rate=30" \
        -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$SRC"
fi

expected=$((FRAMES * FRAME_BYTES))
actual=$(wc -c < "$SRC")
[ "$actual" -eq "$expected" ] || {
    echo "soak: source size mismatch: expected $expected, got $actual" >&2
    exit 1
}

rm -f "$BIT" "$DEC"
start=$(date +%s)
"$ROOT/build/tcenc" -w "$W" -h "$H" -n "$FRAMES" -q "$QP" \
    --v2 --entropy -t "$THREADS" -p 0 -o "$BIT" "$SRC" >"$ENC_LOG" 2>&1
enc_end=$(date +%s)
"$ROOT/build/tcdec" "$BIT" "$DEC" >"$DEC_LOG" 2>&1
dec_end=$(date +%s)

actual=$(wc -c < "$DEC")
[ "$actual" -eq "$expected" ] || {
    echo "soak: decoded size mismatch: expected $expected, got $actual" >&2
    exit 1
}

enc_seconds=$((enc_end - start))
dec_seconds=$((dec_end - enc_end))
enc_fps=0
dec_fps=0
[ "$enc_seconds" -gt 0 ] && enc_fps=$((FRAMES / enc_seconds))
[ "$dec_seconds" -gt 0 ] && dec_fps=$((FRAMES / dec_seconds))

echo "soak: PASS frames=$FRAMES source_bytes=$expected packet_bytes=$(wc -c < "$BIT") decoded_bytes=$actual"
echo "soak: encode_seconds=$enc_seconds encode_fps~$enc_fps decode_seconds=$dec_seconds decode_fps~$dec_fps (serial decoder)"
echo "soak: logs=$ENC_LOG,$DEC_LOG"
