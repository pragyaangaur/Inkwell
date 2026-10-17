#!/usr/bin/env python3
"""Turns a raw 8-bit Kindle framebuffer dump (pitch 1088, 1072x1448 visible) into a PNG."""
import sys, zlib, struct
raw = open(sys.argv[1], 'rb').read()
W, H, P = 1072, 1448, 1088
rows = b''.join(b'\0' + raw[y * P:y * P + W] for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
