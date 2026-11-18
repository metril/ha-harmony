#ifndef HP_REMOTE_THEME_H
#define HP_REMOTE_THEME_H

/* Direction B ("Cards") design tokens: dark blue-grey ground, filled cards, teal accent.
 * Every colour is exactly representable in RGB565 (R,B multiples of 8; G multiple of 4), so what
 * is specified is what the panel shows. Styles are const (flash, no heap, no init). */
#include "lvgl.h"

/* 0xRRGGBB tokens */
#define RA_HEX_BG 0x101820
#define RA_HEX_S1 0x182430 /* card / pill surface */
#define RA_HEX_S2 0x202C38 /* button surface, toast base, hairlines */
#define RA_HEX_S3 0x304050 /* pressed fill */
#define RA_HEX_TX 0xF8FCF8
#define RA_HEX_TX2 0xA8B4C0
#define RA_HEX_TX3 0x687888
#define RA_HEX_ACC 0x20C8B0 /* teal: the active/primary thing */
#define RA_HEX_ONACC 0x081C18
#define RA_HEX_WARM 0xF8C040 /* "on" lights */
#define RA_HEX_ONWARM 0x101820
#define RA_HEX_BAD 0xF85848
#define RA_HEX_ONBAD 0x100808

#define RA_RGB(hex) LV_COLOR_MAKE(((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF)
#define RA_COLOR(hex) lv_color_hex(hex)

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

/* Fonts: Ubuntu, 4 bpp, Latin-1 (tools/gen_remote_fonts.sh) */
LV_FONT_DECLARE(ra_font_b20); /* display: state screens, tiles */
LV_FONT_DECLARE(ra_font_b18); /* top-bar title */
LV_FONT_DECLARE(ra_font_m16); /* body: card labels */
LV_FONT_DECLARE(ra_font_b13); /* narrow buttons */
LV_FONT_DECLARE(ra_font_r12); /* caption, battery % */

extern const lv_style_t ra_st_screen, ra_st_bar, ra_st_btn, ra_st_btn_pr, ra_st_card, ra_st_card_pr, ra_st_toast;
extern const lv_style_t ra_st_txt_title, ra_st_txt_display, ra_st_txt_body, ra_st_txt_btn, ra_st_txt_cap;
extern const lv_style_t ra_st_txt_cap_bad, ra_st_txt_ontoast, ra_st_ink; /* ink: bare coloured rect (bars, fill) */

#endif
