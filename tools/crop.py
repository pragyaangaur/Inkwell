#!/usr/bin/env python3
"""Crops out/fb.raw (the last screenshot) to a PNG. Usage: crop.py x0 y0 x1 y1 out.png"""
import sys, zlib, struct
x0, y0, x1, y1 = map(int, sys.argv[1:5])
raw = open('out/fb.raw', 'rb').read()
P = 1088
rows = b''.join(b'\0' + raw[y * P + x0:y * P + x1] for y in range(y0, y1))
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[5], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', x1 - x0, y1 - y0, 8, 0, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows)) + ch(b'IEND', b''))
