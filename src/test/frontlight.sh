#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -f "$WORK/frontlight"; rmdir "$WORK"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -fno-omit-frame-pointer \
    -fsanitize=address,undefined -I"$ROOT/third_party/stb" \
    "$ROOT/src/test/frontlight.c" -lm -o "$WORK/frontlight"
for board in 0 1; do
    ASAN_OPTIONS=detect_leaks=1 "$WORK/frontlight" "$ROOT/fonts/Inter-SemiBold.ttf" "$board"
done
