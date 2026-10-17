#!/bin/sh
# Rebuilds downloads/ui-fonts, Inkwell's type set, from the upstream fonts:
# static instances of the variable fonts (stb_truetype only draws a variable
# font's default) and lining figures for Cormorant. Needs fonttools.
set -e
cd "$(dirname "$0")/../downloads/fontpk"
O=../ui-fonts
mkdir -p $O
inst() { python3 -m fontTools.varLib.instancer -q "$@"; }
inst usr/share/fonts/inter/InterVariable.ttf wght=400 opsz=14 -o $O/Inter-Regular.ttf
inst usr/share/fonts/inter/InterVariable.ttf wght=600 opsz=14 -o $O/Inter-SemiBold.ttf
inst usr/share/fonts/inter/InterVariable.ttf wght=800 opsz=32 -o $O/Inter-ExtraBold.ttf
inst CormorantGaramond.ttf wght=500 -o $O/Cormorant-Medium.ttf
inst CormorantGaramond.ttf wght=700 -o $O/Cormorant-Bold.ttf
inst CormorantGaramond-Italic.ttf wght=500 -o $O/Cormorant-Italic.ttf
inst Caveat.ttf wght=500 -o $O/Caveat-Medium.ttf
cp usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf $O/
python3 ../../tools/lining.py $O/Cormorant-*.ttf
