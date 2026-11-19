#!/bin/sh
# Fails when remote/app (except theme.c, theme_presets.c and generated font/icon data) holds a colour literal.
dir=${1:?remote/app dir}
bad=$(grep -rnE 'lv_color_hex|LV_COLOR_MAKE|lv_color_make|0x[0-9A-Fa-f]{6}\b' "$dir" --include='*.c' --include='*.h' |
  grep -vE '/(theme|theme_presets)\.[ch]:|/fonts/|/icons\.h:')
if [ -n "$bad" ]; then
  echo "colour literals outside theme.c / theme_presets.c:"
  echo "$bad"
  exit 1
fi
