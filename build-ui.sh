#!/bin/sh
# Cross-compiles our own userspace programs (home screen, splash, tap) and the
# games into out/.
set -e
cd "$(dirname "$0")"
mkdir -p out
UI="-Ithird_party/stb -Wno-unused-function -s"
for p in home splash wifi settings notes sketch browser term chess; do tools/kcc $UI -o out/$p src/ui/$p.c; done
tools/kcc -s -o out/tap src/test/tap.c
tools/kcc $UI -o out/quill src/chat/quill.c
./build.sh >/dev/null
