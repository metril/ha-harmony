#!/bin/sh
# Regenerate the remote UI fonts (Ubuntu, UFL 1.0) as LVGL C arrays: 4 bpp, no compression, no kerning.
# Glyphs: Basic Latin, Latin-1 Supplement, and U+2013 U+2014 U+2018-201D U+2026 (dashes, quotes, ellipsis).
# Needs node/npx (lv_font_conv 1.5.3 is fetched on demand). Usage: tools/gen_remote_fonts.sh [fontdir]
set -eu
FD=${1:-/usr/share/fonts/truetype/ubuntu}
OUT=$(dirname "$0")/../remote/app/fonts
RANGE=0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2018-0x201D,0x2026
gen() { # name face size
  npx -y lv_font_conv@1.5.3 --font "$FD/$2" --size "$3" --bpp 4 --format lvgl --no-compress --no-kerning \
    -r "$RANGE" --lv-font-name "$1" --lv-include lvgl.h -o "$OUT/$1.c"
}
mkdir -p "$OUT"
gen ra_font_b20 Ubuntu-B.ttf 20
gen ra_font_b18 Ubuntu-B.ttf 18
gen ra_font_m16 Ubuntu-M.ttf 16
gen ra_font_b13 Ubuntu-B.ttf 13
gen ra_font_r12 Ubuntu-R.ttf 12
