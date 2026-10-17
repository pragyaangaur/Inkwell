#!/bin/sh
# Saves a PNG of the Kindle's screen to out/<name>.png.
cd "$(dirname "$0")/.."
./ssh.sh 'dd if=/dev/fb0 bs=1088 count=1448 2>/dev/null' > out/fb.raw && python3 tools/fbshot.py out/fb.raw "out/${1:-shot}.png"
