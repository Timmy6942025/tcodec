#!/bin/bash
# ────────────────────────────────────────────────────────────────
# fetch_corpus.sh — build the TCodec real-content benchmark corpus
#
# Reads benchmark_v2_real/CORPUS.tsv and produces, for every clip id,
#   benchmark_v2_real/raw/<id>.mkv   (FFV1 lossless, exact pixels)
# plus benchmark_v2_real/MANIFEST.sha256.
#
# Sources are public Xiph.org media (media.xiph.org). Large sources are
# fetched as byte-range prefixes and truncated mid-frame on purpose —
# only the leading, fully decoded frames are used.
#
# Usage: tools/fetch_corpus.sh [id ...]     (no args = all clips)
# ────────────────────────────────────────────────────────────────
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/benchmark_v2_real/CORPUS.tsv"
RAW="$ROOT/benchmark_v2_real/raw"
TMP="${TCODEC_TMP:-/tmp/tcfetch}"
mkdir -p "$RAW" "$TMP"

want=("$@")

want_id() {
    [ ${#want[@]} -eq 0 ] && return 0
    local wid
    for wid in "${want[@]}"; do [ "$wid" = "$1" ] && return 0; done
    return 1
}

# generated screen-content clip: scrolling source code + UI chrome
gen_screen() {
    local out="$1" w="$2" h="$3" fps="$4" dur="$5"
    local txt="$TMP/screen.txt"
    if [ ! -f "$txt" ]; then
        cat "$ROOT"/src/*.c | head -4000 | sed 's/\t/    /g' | cut -c1-96 > "$txt"
    fi
    local font=""
    for f in /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
             /usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf \
             /usr/share/fonts/TTF/DejaVuSansMono.ttf; do
        [ -f "$f" ] && font="$f" && break
    done
    if [ -z "$font" ]; then
        echo "  !! no monospace TTF found — screen_ui clip skipped"
        return 1
    fi
    ffmpeg -nostdin -v error -y \
        -f lavfi -i "color=c=0x1e1e2e:s=${w}x${h}:r=${fps}:d=${dur}" \
        -vf "drawbox=x=0:y=0:w=${w}:h=28:color=0x313244:t=fill,\
drawbox=x=0:y=28:w=220:h=$((h-28)):color=0x181825:t=fill,\
drawtext=fontfile=${font}:text='tcodec — src/encoder.c':x=240:y=6:fontsize=15:fontcolor=0xcdd6f4,\
drawtext=fontfile=${font}:textfile=${txt}:x=236:y=h-40*t:fontsize=14:fontcolor=0xa6e3a1:line_spacing=4,\
drawbox=x=232:y=28:w=2:h=$((h-28)):color=0x89b4fa:t=fill" \
        -pix_fmt yuv420p -c:v ffv1 -level 3 "$out" 2>&1 | tail -3
}

fetch_one() {
    local id="$1" url="$2" bytes="$3" seek="$4" dur="$5" w="$6" h="$7" fps="$8"
    local out="$RAW/$id.mkv"
    if [ -s "$out" ]; then echo "  == $id already present"; return 0; fi
    echo "  -> $id  ($url)"
    local vf="scale=${w}:${h}:flags=lanczos,format=yuv420p"

    if [ "$url" = "generated:screen" ]; then
        gen_screen "$out" "$w" "$h" "$fps" "$dur"
        return $?
    fi

    case "$url" in
    *.xz)
        curl -sS --retry 3 </dev/null -r "0-$((bytes-1))" "$url" 2>/dev/null \
          | xz -dc 2>/dev/null \
          | ffmpeg -nostdin -v error -f yuv4mpegpipe -i pipe:0 -ss "$seek" -t "$dur" \
                   -vf "$vf" -r "$fps" -c:v ffv1 -level 3 -y "$out" 2>&1 | tail -2
        ;;
    *.y4m)
        local range=""
        [ "$bytes" != "0" ] && range="-r 0-$((bytes-1))"
        # shellcheck disable=SC2086
        curl -sS --retry 3 </dev/null $range "$url" 2>/dev/null \
          | ffmpeg -nostdin -v error -f yuv4mpegpipe -i pipe:0 -ss "$seek" -t "$dur" \
                   -vf "$vf" -r "$fps" -c:v ffv1 -level 3 -y "$out" 2>&1 | tail -2
        ;;
    *)
        local base; base="$TMP/$(basename "$url")"
        [ -s "$base" ] || curl -sS --retry 3 </dev/null -o "$base" "$url"
        ffmpeg -nostdin -v error -ss "$seek" -i "$base" -t "$dur" \
               -vf "$vf" -r "$fps" -c:v ffv1 -level 3 -y "$out" 2>&1 | tail -2
        ;;
    esac
    [ -s "$out" ] || { echo "  !! $id FAILED"; rm -f "$out"; return 1; }
    local nf; nf=$(ffprobe -v error -count_frames -select_streams v:0 \
                   -show_entries stream=nb_read_frames -of csv=p=0 "$out" 2>/dev/null)
    echo "     $id: $(stat -c%s "$out") bytes, ${nf} frames"
}

while IFS=$'\t' read -r -u 3 id url bytes seek dur w h fps class; do
    case "$id" in ''|'#'*|id) continue;; esac
    want_id "$id" || continue
    fetch_one "$id" "$url" "$bytes" "$seek" "$dur" "$w" "$h" "$fps"
done 3< "$CORPUS"

echo "── manifest ──"
( cd "$RAW" && sha256sum ./*.mkv 2>/dev/null ) > "$ROOT/benchmark_v2_real/MANIFEST.sha256"
wc -l < "$ROOT/benchmark_v2_real/MANIFEST.sha256" | xargs echo "clips in manifest:"
