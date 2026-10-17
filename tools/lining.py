#!/usr/bin/env python3
"""Points a font's digit code points at its lining figures (glyphs named
'one.lf' and so on), since stb_truetype can't apply the lnum feature.
Usage: lining.py font.ttf [more.ttf ...] (edits in place)"""
import sys
from fontTools.ttLib import TTFont
digits = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']
for path in sys.argv[1:]:
    f = TTFont(path)
    names = set(f.getGlyphOrder())
    for table in f['cmap'].tables:
        if not table.isUnicode():
            continue
        for i, d in enumerate(digits):
            if d + '.lf' in names:
                table.cmap[0x30 + i] = d + '.lf'
    f.save(path)
    print('lining figures in', path)
