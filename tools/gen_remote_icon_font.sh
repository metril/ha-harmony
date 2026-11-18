#!/bin/sh
# Regenerate the remote icon fonts (our own line icons, assets/icons/*.svg) as LVGL C arrays:
# strokes -> fills (picosvg) -> TTF (fontTools) -> lv_font_conv, 4 bpp, no compression, sizes 20/24/32.
# Glyph for icon id N is U+E000+N (proto/icons.yaml). Needs uv and node/npx (lv_font_conv 1.5.3).
# Usage: tools/gen_remote_icon_font.sh [ttf-out-for-inspection]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../remote/app/fonts
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
uv run --quiet --no-project --with picosvg --with fonttools --with pyyaml \
  python "$HERE/icons_to_ttf.py" "$TMP/ra_icons.ttf"
[ -z "${1:-}" ] || cp "$TMP/ra_icons.ttf" "$1"
LAST=$(awk '/^  - \{id:/ {gsub(/[^0-9]/, "", $3); n=$3} END {print n}' "$HERE/../proto/icons.yaml")
RANGE=$(printf '0xE001-0x%X' $((0xE000 + LAST)))
mkdir -p "$OUT"
for s in 20 24 32; do
  npx -y lv_font_conv@1.5.3 --font "$TMP/ra_icons.ttf" --size "$s" --bpp 4 --format lvgl --no-compress \
    --no-kerning -r "$RANGE" --lv-font-name "ra_icons_$s" --lv-include lvgl.h -o "$OUT/ra_icons_$s.c"
done
