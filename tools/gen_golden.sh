#!/bin/bash
# gen_golden.sh — Generate golden/conformance corpus for TCodec
#
# Encodes standard test patterns at multiple QPs across every supported
# bitstream version and entropy mode, stores the bitstreams and decoded
# outputs, and writes SHA-256 hashes for conformance verification.
#
#   ./tools/gen_golden.sh [golden_dir]
#
# The manifest (MANIFEST.sha256) is committed; the binary streams are
# gitignored and regenerated locally. `make test` regenerates and
# verifies before running the suite.
set -e

GOLDEN_DIR="${1:-golden}"
TCENC="./build/tcenc"
TCDEC="./build/tcdec"

if [ ! -x "$TCENC" ] || [ ! -x "$TCDEC" ]; then
    echo "Error: $TCENC or $TCDEC not found. Run 'make' first."
    exit 1
fi

mkdir -p "$GOLDEN_DIR"

# Generate raw YUV test clips
gen_yuv() {
    local name=$1 w=$2 h=$3 pattern=$4
    local yuv="$GOLDEN_DIR/${name}_${w}x${h}.yuv"
    python3 -c "
import sys
path, w, h, pattern = '$yuv', $w, $h, '$pattern'
with open(path, 'wb') as f:
    for row in range(h):
        row_data = bytearray(w)
        for col in range(w):
            if pattern == 'gradient':
                v = (row * 255 // h + col * 255 // w) // 2
            elif pattern == 'checkerboard':
                v = 255 if ((col // 16) + (row // 16)) % 2 else 0
            elif pattern == 'noise':
                import random
                random.seed(row * w + col)
                v = random.randint(0, 255)
            elif pattern == 'horizontal_lines':
                v = 255 if row % 16 < 8 else 0
            elif pattern == 'vertical_lines':
                v = 255 if col % 16 < 8 else 0
            elif pattern == 'diagonal':
                v = 255 if (row + col) % 32 < 16 else 0
            else:
                v = 128
            row_data[col] = v
        f.write(row_data)
    cb_size = (w // 2) * (h // 2)
    f.write(bytes([128]) * cb_size)
    f.write(bytes([128]) * cb_size)
"
    echo "$yuv"
}

# Generate test clips
echo "Generating test YUV clips..."
gen_yuv gradient 128 128 gradient
gen_yuv checkerboard 128 128 checkerboard
gen_yuv noise 128 128 noise
gen_yuv hlines 128 128 horizontal_lines
gen_yuv vlines 128 128 vertical_lines
gen_yuv diagonal 128 128 diagonal
gen_yuv gradient 320 240 gradient
gen_yuv gradient 96 80 gradient

# Encode each clip at multiple QPs across all bitstream versions
QPS="22 32 42"
HASH_FILE="$GOLDEN_DIR/MANIFEST.sha256"
> "$HASH_FILE"

echo "Encoding golden bitstreams..."
for yuv in "$GOLDEN_DIR"/*.yuv; do
    base=$(basename "$yuv" .yuv)
    case "$base" in *_dec) continue ;; esac   # skip decoded outputs
    dims=$(echo "$base" | grep -oP '\d+x\d+')
    w=$(echo "$dims" | cut -dx -f1)
    h=$(echo "$dims" | cut -dx -f2)

    for qp in $QPS; do
        # v0: legacy 12-byte-header bitstream (frozen)
        tcname="${base}_qp${qp}_v0.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" --bs-version 0 \
            -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 \
            || { echo "  FAILED: $tcname"; exit 1; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"

        # v1: default Exp-Golomb path
        tcname="${base}_qp${qp}_v1.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" --bs-version 1 \
            -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 \
            || { echo "  FAILED: $tcname"; exit 1; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"

        # v1 entropy: context-modeled range coder
        tcname="${base}_qp${qp}_v1e.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" --bs-version 1 --entropy \
            -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 \
            || { echo "  FAILED: $tcname"; exit 1; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"

        # v2: quadtree raw syntax
        tcname="${base}_qp${qp}_v2.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" --v2 \
            -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 \
            || { echo "  FAILED: $tcname"; exit 1; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"

        # v2 entropy: quadtree + range coder
        tcname="${base}_qp${qp}_v2e.tcv"
        echo "  Encoding $tcname (w=$w h=$h qp=$qp)..."
        "$TCENC" -w "$w" -h "$h" -q "$qp" --v2 --entropy \
            -o "$GOLDEN_DIR/$tcname" "$yuv" 2>&1 \
            || { echo "  FAILED: $tcname"; exit 1; }
        hash=$(sha256sum "$GOLDEN_DIR/$tcname" | cut -d' ' -f1)
        echo "$hash  $tcname" >> "$HASH_FILE"
    done
done

# Decode every conformance stream and hash the decoded output.
# Byte-identical re-decode is asserted both here (manifest) and in
# the C suite (test_golden_decode).
echo "Decoding conformance streams..."
for tc in "$GOLDEN_DIR"/*.tcv; do
    dec="${tc%.tcv}_dec.yuv"
    "$TCDEC" "$tc" "$dec" 2>&1 || { echo "  DECODE FAILED: $tc"; exit 1; }
    hash=$(sha256sum "$dec" | cut -d' ' -f1)
    echo "$hash  $(basename "$dec")" >> "$HASH_FILE"
done

echo ""
echo "Golden corpus generated in $GOLDEN_DIR/"
echo "Manifest: $HASH_FILE"
(cd "$GOLDEN_DIR" && sha256sum -c MANIFEST.sha256) >/dev/null && echo "Manifest verified OK"
