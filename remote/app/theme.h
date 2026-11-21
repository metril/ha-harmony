#ifndef HP_REMOTE_THEME_H
#define HP_REMOTE_THEME_H

/* Direction B ("Cards") geometry, fonts and the runtime styles built from the current theme (proto/themes.yaml).
 * Every colour is exactly representable in RGB565, so what is specified is what the panel shows.
 * No colour literal lives outside theme.c / theme_presets.c (tests/c/test_theme_literals.sh). */
#include <stddef.h>

#include "lvgl.h"
#include "remote/app/theme_derive.h"
#include "remote/app/theme_presets.h"

/* The theme is a runtime value; every colour comes from ra_theme_cur() (0xRRGGBB, RGB565-exact). */
const ra_theme_t *ra_theme_cur(void);
lv_color_t ra_color(uint32_t rgb);
uint32_t ra_brand_rgb(uint8_t icon); /* brand logo colour (RRGGBB) of an icon id, 0 when not a brand logo */

/* Initialise the styles (after lv_init): default preset, then HARMONY_THEME=<preset key|id> if set (a bench aid).
 * Called by ra_ui_init. */
void ra_theme_init(void);
/* Switch the whole UI to a built-in preset / a 99-byte wire theme. 0 ok; <0 error, current theme kept.
 * Styles are updated in place and everything repainted: no allocation, no widget re-creation, cheap enough
 * for every page show. */
int ra_theme_apply_preset(uint8_t id);
int ra_theme_apply_wire(const uint8_t *bytes, size_t n);
/* Called after every successful apply (the UI recolours its per-widget colours). */
void ra_theme_set_listener(void (*cb)(void *), void *ctx);

/* Palette entry 1..RA_PALETTE_N of the current theme; NULL for 0 / out of range. */
const ra_swatch_t *ra_theme_palette(unsigned idx);
/* A free RGB565 colour -> fill/ink/pressed from the current TX/BG; *outline = 1 when it needs a 1 px TX3 outline. */
void ra_theme_free_colour(uint16_t rgb565, ra_swatch_t *out, int *outline);
/* Per-widget colours with no shared style and no per-frame allocation: fill, pressed fill and optional outline
 * on `o`; ink on `label` (may be NULL, or == o). */
void ra_theme_local(lv_obj_t *o, lv_obj_t *label, const ra_swatch_t *s, int outline);

/* Geometry (4 px grid). The top bar is RA_BAR_H; the page grid fills the 240x288 content area. */
#define RA_SCREEN_W 240
#define RA_SCREEN_H 320
#define RA_BAR_H 32
#define RA_CONTENT_H (RA_SCREEN_H - RA_BAR_H)
#define RA_MARGIN 8 /* screen edge to widget */
#define RA_GAP 8    /* widget to widget (4 px inset each side of a cell edge) */
#define RA_RADIUS_BTN 10
#define RA_RADIUS_CARD 14
#define RA_TOAST_H 56
#define RA_TOAST_MS 5000
#define RA_LOWBATT_PCT 10
#define RA_LOWBATT_REARM_PCT 13

/* Icon fonts (tools/gen_remote_icon_font.sh): glyph U+E000 + icon id, see icons.h */
LV_FONT_DECLARE(ra_icons_20);
LV_FONT_DECLARE(ra_icons_24);
LV_FONT_DECLARE(ra_icons_32);

/* Fonts: Ubuntu, 4 bpp, Latin-1 (tools/gen_remote_fonts.sh) */
LV_FONT_DECLARE(ra_font_b20); /* display: state screens, tiles */
LV_FONT_DECLARE(ra_font_b18); /* top-bar title */
LV_FONT_DECLARE(ra_font_m16); /* body: card labels */
LV_FONT_DECLARE(ra_font_b13); /* narrow buttons */
LV_FONT_DECLARE(ra_font_r12); /* caption, battery % */

extern lv_style_t ra_st_screen, ra_st_bar, ra_st_btn, ra_st_btn_pr, ra_st_card, ra_st_card_pr, ra_st_toast;
extern lv_style_t ra_st_txt_title, ra_st_txt_display, ra_st_txt_body, ra_st_txt_btn, ra_st_txt_cap;
extern lv_style_t ra_st_txt_ontoast, ra_st_ink; /* ink: bare coloured rect (bars, fill) */

#endif
