#include "remote/app/theme.h"

/* LV_STYLE_CONST_INIT stores the const property array in a non-const pointer (LVGL does not write it). */
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

#define C(hex) RA_RGB(hex)

/* Surfaces. LV_STYLE_CONST_* live in flash; shadows/outlines are never set (no cost, no shadow). */
static const lv_style_const_prop_t p_screen[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_BG)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_PAD_TOP(0),
    LV_STYLE_CONST_PAD_BOTTOM(0), LV_STYLE_CONST_PAD_LEFT(0), LV_STYLE_CONST_PAD_RIGHT(0), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_screen, p_screen);

static const lv_style_const_prop_t p_bar[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_BG)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_BORDER_WIDTH(1),
    LV_STYLE_CONST_BORDER_SIDE(LV_BORDER_SIDE_BOTTOM), LV_STYLE_CONST_BORDER_COLOR(C(RA_HEX_S2)),
    LV_STYLE_CONST_PAD_TOP(0), LV_STYLE_CONST_PAD_BOTTOM(0), LV_STYLE_CONST_PAD_LEFT(0), LV_STYLE_CONST_PAD_RIGHT(0),
    LV_STYLE_CONST_RADIUS(0), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_bar, p_bar);

static const lv_style_const_prop_t p_btn[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_S2)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_RADIUS(RA_RADIUS_BTN),
    LV_STYLE_CONST_BORDER_WIDTH(0), LV_STYLE_CONST_PAD_TOP(0), LV_STYLE_CONST_PAD_BOTTOM(0),
    LV_STYLE_CONST_PAD_LEFT(4), LV_STYLE_CONST_PAD_RIGHT(4), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_btn, p_btn);

static const lv_style_const_prop_t p_card[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_S1)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_RADIUS(RA_RADIUS_CARD),
    LV_STYLE_CONST_BORDER_WIDTH(0), LV_STYLE_CONST_PAD_TOP(0), LV_STYLE_CONST_PAD_BOTTOM(0),
    LV_STYLE_CONST_PAD_LEFT(4), LV_STYLE_CONST_PAD_RIGHT(4), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_card, p_card);

/* Pressed: lighter fill and a 2 px white edge, drawn inside the widget box (no outline, no growth). */
static const lv_style_const_prop_t p_press[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_S3)), LV_STYLE_CONST_BORDER_WIDTH(2), LV_STYLE_CONST_BORDER_COLOR(C(RA_HEX_TX)),
    LV_STYLE_CONST_BORDER_OPA(LV_OPA_COVER), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_btn_pr, p_press);
LV_STYLE_CONST_INIT(ra_st_card_pr, p_press);

static const lv_style_const_prop_t p_toast[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_BAD)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_RADIUS(RA_RADIUS_CARD),
    LV_STYLE_CONST_BORDER_WIDTH(0), LV_STYLE_CONST_PAD_TOP(0), LV_STYLE_CONST_PAD_BOTTOM(0),
    LV_STYLE_CONST_PAD_LEFT(0), LV_STYLE_CONST_PAD_RIGHT(0), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_toast, p_toast);

static const lv_style_const_prop_t p_ink[] = {
    LV_STYLE_CONST_BG_COLOR(C(RA_HEX_TX2)), LV_STYLE_CONST_BG_OPA(LV_OPA_COVER), LV_STYLE_CONST_BORDER_WIDTH(0),
    LV_STYLE_CONST_RADIUS(1), LV_STYLE_CONST_PAD_TOP(0), LV_STYLE_CONST_PAD_BOTTOM(0), LV_STYLE_CONST_PAD_LEFT(0),
    LV_STYLE_CONST_PAD_RIGHT(0), LV_STYLE_CONST_PROPS_END};
LV_STYLE_CONST_INIT(ra_st_ink, p_ink);

/* Text. */
#define TXT_STYLE(name, font, hex, align)                                                                        \
  static const lv_style_const_prop_t p_##name[] = {LV_STYLE_CONST_TEXT_FONT(&font),                              \
                                                   LV_STYLE_CONST_TEXT_COLOR(C(hex)),                            \
                                                   LV_STYLE_CONST_TEXT_ALIGN(align), LV_STYLE_CONST_PROPS_END}; \
  LV_STYLE_CONST_INIT(name, p_##name)
TXT_STYLE(ra_st_txt_title, ra_font_b18, RA_HEX_TX, LV_TEXT_ALIGN_LEFT);
TXT_STYLE(ra_st_txt_display, ra_font_b20, RA_HEX_TX, LV_TEXT_ALIGN_CENTER);
TXT_STYLE(ra_st_txt_body, ra_font_m16, RA_HEX_TX, LV_TEXT_ALIGN_CENTER);
TXT_STYLE(ra_st_txt_btn, ra_font_b13, RA_HEX_TX, LV_TEXT_ALIGN_CENTER);
TXT_STYLE(ra_st_txt_cap, ra_font_r12, RA_HEX_TX2, LV_TEXT_ALIGN_CENTER);
TXT_STYLE(ra_st_txt_cap_bad, ra_font_r12, RA_HEX_BAD, LV_TEXT_ALIGN_RIGHT);
TXT_STYLE(ra_st_txt_ontoast, ra_font_m16, RA_HEX_ONBAD, LV_TEXT_ALIGN_LEFT);
