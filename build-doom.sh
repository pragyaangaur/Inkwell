#!/bin/sh
# Cross-compiles Doom (doomgeneric plus our Kindle port) into out/doom.
set -e
cd "$(dirname "$0")"
mkdir -p out
tools/kcc -s -O2 -std=gnu99 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -DLINUX \
    -Isrc/doom -Ithird_party/stb -w -o out/doom src/doom/*.c
ls -l out/doom
