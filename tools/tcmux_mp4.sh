#!/bin/sh
# tcmux_mp4.sh — playable ISO-BMFF compatibility bridge for TCV.
#
# TCMX/TCMF preserve native TCV access units but are intentionally private
# transport formats. This bridge makes the stream playable by ordinary
# FFmpeg/ffplay by decoding TCV and re-encoding the frames as H.264 in MP4.
# It is not a native TCodec MP4 sample entry.
#
# Usage:
#   tcmux_mp4.sh mux     -i input.tcv -o output.mp4 -w 1280 -h 720 -f 30
#   tcmux_mp4.sh demux   -i input.mp4 -o output.yuv
#   tcmux_mp4.sh segment -i input.tcv -o directory -w 1280 -h 720 -f 30 -s 2
#
# Optional environment: TCDEC, FFMPEG, FFPROBE.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TCDEC=${TCDEC:-$ROOT/build/tcdec}
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}

usage() {
    echo "Usage: $0 mux -i input.tcv -o output.mp4 -w W -h H -f FPS" >&2
    echo "       $0 demux -i input.mp4 -o output.yuv" >&2
    echo "       $0 segment -i input.tcv -o directory -w W -h H -f FPS -s SECONDS [-p playlist.m3u8]" >&2
}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "tcmux_mp4: missing executable: $1" >&2
        exit 1
    }
}

value() {
    key=$1
    shift
    while [ "$#" -gt 1 ]; do
        if [ "$1" = "$key" ]; then echo "$2"; return 0; fi
        shift 2
    done
    return 1
}

positive() {
    case "$1" in ''|*[!0-9]*|0) return 1;; esac
}

mode=${1:-}
[ -n "$mode" ] || { usage; exit 2; }
shift
in=$(value -i "$@" || true)
out=$(value -o "$@" || true)
[ -n "$in" ] && [ -n "$out" ] || { usage; exit 2; }

case "$mode" in
mux|segment)
    w=$(value -w "$@" || true)
    h=$(value -h "$@" || true)
    fps=$(value -f "$@" || true)
    [ -n "$w" ] && [ -n "$h" ] && [ -n "$fps" ] || { usage; exit 2; }
    positive "$w" && positive "$h" && positive "$fps" || {
        echo "tcmux_mp4: dimensions/fps must be positive integers" >&2
        exit 2
    }
    need "$TCDEC"
    need "$FFMPEG"
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/tcmux-mp4.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT HUP INT TERM
    yuv=$tmp/decoded.yuv
    "$TCDEC" "$in" "$yuv" >"$tmp/tcdec.log" 2>&1 || {
        cat "$tmp/tcdec.log" >&2
        exit 1
    }
    if [ "$mode" = mux ]; then
        tmpmp4=$tmp/output.mp4
        "$FFMPEG" -nostdin -v error -y \
            -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -r "$fps" -i "$yuv" \
            -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p \
            -movflags +faststart "$tmpmp4"
        out_dir=$(dirname "$out")
        mkdir -p "$out_dir"
        if [ -e "$out" ]; then
            echo "tcmux_mp4: refusing to overwrite existing output: $out" >&2
            exit 1
        fi
        staged=$(mktemp "$out_dir/.tcmux_mp4.XXXXXX")
        cp "$tmpmp4" "$staged"
        mv "$staged" "$out"
    else
        seconds=$(value -s "$@" || true)
        playlist=$(value -p "$@" || true)
        [ -n "$seconds" ] && positive "$seconds" || {
            echo "tcmux_mp4: segment duration must be positive" >&2
            exit 2
        }
        if [ -e "$out" ]; then
            echo "tcmux_mp4: refusing to overwrite existing segment directory: $out" >&2
            exit 1
        fi
        playlist=${playlist:-$out/playlist.m3u8}
        if [ "$playlist" != "$out/playlist.m3u8" ]; then
            playlist_dir=$(dirname "$playlist")
            mkdir -p "$playlist_dir"
            if [ -e "$playlist" ]; then
                echo "tcmux_mp4: refusing to overwrite existing playlist: $playlist" >&2
                exit 1
            fi
        fi
        parent=$(dirname "$out")
        mkdir -p "$parent"
        stage=$(mktemp -d "$parent/.tcmux_mp4_segments.XXXXXX")
        trap 'rm -rf "$tmp" "$stage"' EXIT HUP INT TERM
        # HLS fMP4 output is ISO-BMFF and can be consumed by ffplay/FFmpeg.
        # Generate the complete directory off to the side, then publish it
        # with one rename so a failed encode cannot leave partial segments.
        "$FFMPEG" -nostdin -v error -y \
            -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -r "$fps" -i "$yuv" \
            -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p \
            -force_key_frames "expr:gte(t,n_forced*${seconds})" \
            -f hls -hls_time "$seconds" -hls_playlist_type vod \
            -hls_segment_type fmp4 -hls_fmp4_init_filename init.mp4 \
            -hls_segment_filename "$stage/seg_%04d.m4s" "$stage/playlist.m3u8"
        test -s "$stage/playlist.m3u8"
        test -s "$stage/init.mp4"
        test -s "$stage/seg_0000.m4s"
        mv "$stage" "$out"
        if [ "$playlist" != "$out/playlist.m3u8" ]; then
            staged_playlist=$(mktemp "$playlist_dir/.tcmux_playlist.XXXXXX")
            if ! cp "$out/playlist.m3u8" "$staged_playlist" || ! mv "$staged_playlist" "$playlist"; then
                rm -rf "$out" "$staged_playlist"
                exit 1
            fi
        fi
    fi
    ;;
demux)
    need "$FFMPEG"
    need "$FFPROBE"
    dims=$($FFPROBE -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=p=0:s=x "$in")
    case "$dims" in
        ''|*[!0-9x]*) echo "tcmux_mp4: cannot determine video dimensions" >&2; exit 1;;
    esac
    out_dir=$(dirname "$out")
    mkdir -p "$out_dir"
    if [ -e "$out" ]; then
        echo "tcmux_mp4: refusing to overwrite existing output: $out" >&2
        exit 1
    fi
    staged=$(mktemp "$out_dir/.tcmux_mp4.XXXXXX")
    trap 'rm -f "$staged"' EXIT HUP INT TERM
    "$FFMPEG" -nostdin -v error -y -i "$in" \
        -pix_fmt yuv420p -f rawvideo "$staged"
    mv "$staged" "$out"
    trap - EXIT HUP INT TERM
    ;;
*)
    usage
    exit 2
    ;;
esac
