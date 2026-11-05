#!/bin/sh
# End-to-end smoke test for the FFmpeg backend and the CLI.
# The GTK window is intentionally not started here.
#
# Copyright (C) 2026 Telona Hyuga
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/hyuga-convert"

for tool in ffmpeg ffprobe; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL: $tool is required" >&2
        exit 1
    fi
done

if [ ! -x "$BIN" ]; then
    echo "FAIL: build the program first (make)" >&2
    exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hyuga-convert-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf '  ok  %s\n' "$1"
}

bad() {
    fail=$((fail + 1))
    printf 'FAIL  %s\n' "$1" >&2
}

assert_file() {
    if [ -s "$1" ]; then
        ok "$2"
    else
        bad "$2 (missing or empty: $1)"
    fi
}

assert_eq() {
    if [ "$1" = "$2" ]; then
        ok "$3"
    else
        bad "$3 (expected '$2', got '$1')"
    fi
}

video_codec() {
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name -of default=nw=1:nk=1 "$1"
}

video_width() {
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=width -of default=nw=1:nk=1 "$1"
}

video_height() {
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=height -of default=nw=1:nk=1 "$1"
}

frame_count() {
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=nb_frames -of default=nw=1:nk=1 "$1"
}

run_convert() {
    "$BIN" --cli -i "$1" -f "$2" -q "$3" -o "$4" >/dev/null
}

printf 'Creating test media in %s\n' "$WORK"
ffmpeg -v error -y \
    -f lavfi -i "testsrc=size=320x180:rate=15" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -t 1 -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
    -c:a aac -shortest "$WORK/video.mp4"
# FFV1 accepts odd dimensions, letting us exercise the even-size scale filter.
ffmpeg -v error -y \
    -f lavfi -i "testsrc=size=321x181:rate=15" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -t 1 -c:v ffv1 -pix_fmt yuv444p -c:a aac -shortest "$WORK/odd.mkv"
ffmpeg -v error -y -f lavfi -i "testsrc=size=257x199" \
    -frames:v 1 "$WORK/image.png"

printf '\nCLI basics\n'
"$BIN" --cli --list-formats | grep -q 'gif-video' && ok "list-formats"
"$BIN" --cli --version | grep -q '1.0.0' && ok "version"
"$BIN" --cli -i "$WORK/video.mp4" -f webm --dry-run > "$WORK/dry.txt"
grep -q "libvpx-vp9" "$WORK/dry.txt" && ok "dry-run prints the FFmpeg command"
[ ! -e "$WORK/video.webm" ] && ok "dry-run does not write a file"

printf '\nImages\n'
run_convert "$WORK/image.png" jpg high "$WORK/image.jpg"
assert_file "$WORK/image.jpg" "PNG -> JPEG"
assert_eq "$(video_codec "$WORK/image.jpg")" "mjpeg" "JPEG codec"
assert_eq "$(video_width "$WORK/image.jpg")" "257" "JPEG width"
assert_eq "$(video_height "$WORK/image.jpg")" "199" "JPEG height"

run_convert "$WORK/image.png" webp balanced "$WORK/image.webp"
assert_file "$WORK/image.webp" "PNG -> WebP"
assert_eq "$(video_codec "$WORK/image.webp")" "webp" "WebP codec"

run_convert "$WORK/image.png" bmp balanced "$WORK/image.bmp"
assert_file "$WORK/image.bmp" "PNG -> BMP"

run_convert "$WORK/image.png" tiff balanced "$WORK/image.tiff"
assert_file "$WORK/image.tiff" "PNG -> TIFF"

run_convert "$WORK/image.png" gif balanced "$WORK/image.gif"
assert_file "$WORK/image.gif" "PNG -> GIF (still)"

printf '\nVideo\n'
run_convert "$WORK/video.mp4" mp4 balanced "$WORK/video-out.mp4"
assert_file "$WORK/video-out.mp4" "MP4 -> MP4"
assert_eq "$(video_codec "$WORK/video-out.mp4")" "h264" "MP4 codec"

run_convert "$WORK/odd.mkv" mp4 balanced "$WORK/odd-out.mp4"
assert_file "$WORK/odd-out.mp4" "odd-sized video -> MP4"
assert_eq "$(video_width "$WORK/odd-out.mp4")" "320" "odd width rounded to even"
assert_eq "$(video_height "$WORK/odd-out.mp4")" "180" "odd height rounded to even"

run_convert "$WORK/video.mp4" mkv balanced "$WORK/video-out.mkv"
assert_file "$WORK/video-out.mkv" "MP4 -> MKV"

run_convert "$WORK/video.mp4" webm balanced "$WORK/video-out.webm"
assert_file "$WORK/video-out.webm" "MP4 -> WebM"
assert_eq "$(video_codec "$WORK/video-out.webm")" "vp9" "WebM codec"

run_convert "$WORK/video.mp4" avi balanced "$WORK/video-out.avi"
assert_file "$WORK/video-out.avi" "MP4 -> AVI"
assert_eq "$(video_codec "$WORK/video-out.avi")" "mpeg4" "AVI codec"

run_convert "$WORK/video.mp4" mov balanced "$WORK/video-out.mov"
assert_file "$WORK/video-out.mov" "MP4 -> MOV"

run_convert "$WORK/video.mp4" gif balanced "$WORK/video-out.gif"
assert_file "$WORK/video-out.gif" "MP4 -> animated GIF"
[ "$(frame_count "$WORK/video-out.gif")" -gt 1 ] && ok "GIF contains multiple frames"

printf '\nGIF input\n'
run_convert "$WORK/video-out.gif" png balanced "$WORK/frame.png"
assert_file "$WORK/frame.png" "GIF -> PNG (first frame)"
run_convert "$WORK/video-out.gif" mp4 balanced "$WORK/from-gif.mp4"
assert_file "$WORK/from-gif.mp4" "GIF -> MP4"
assert_eq "$(video_codec "$WORK/from-gif.mp4")" "h264" "GIF -> MP4 codec"

printf '\nAutomatic output name\n'
auto_out=$("$BIN" --cli -i "$WORK/image.png" -f webp | sed -n 's/^Готово: //p')
if [ -n "$auto_out" ] && [ -s "$auto_out" ]; then
    ok "output path is generated next to the source"
else
    bad "automatic output path"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
