#include "remote/app/theme.h"

#include <stdlib.h>
#include <string.h>

lv_style_t ra_st_screen, ra_st_bar, ra_st_btn, ra_st_btn_pr, ra_st_card, ra_st_card_pr, ra_st_toast;
lv_style_t ra_st_txt_title, ra_st_txt_display, ra_st_txt_body, ra_st_txt_btn, ra_st_txt_cap;
lv_style_t ra_st_txt_ontoast, ra_st_ink;

static ra_theme_t cur;
static int cur_init, ready;
static void (*listener)(void *);
static void *listener_ctx;

const ra_theme_t *ra_theme_cur(void) {
  if (!cur_init) {
    cur = ra_themes[RA_THEME_DEFAULT];
    cur_init = 1;
  }
  return &cur;
}

lv_color_t ra_color(uint32_t rgb) { return lv_color_hex(rgb); }

#define C(v) ra_color(v)

static void pad(lv_style_t *s, int l, int r) {
  lv_style_set_pad_top(s, 0);
  lv_style_set_pad_bottom(s, 0);
  lv_style_set_pad_left(s, l);
  lv_style_set_pad_right(s, r);
}

static void txt(lv_style_t *s, const lv_font_t *f, uint32_t col, lv_text_align_t a) {
  lv_style_set_text_font(s, f);
  lv_style_set_text_color(s, C(col));
  lv_style_set_text_align(s, a);
}

/* (Re)sets every property: the first call allocates each, later calls overwrite in place. */
static void build(void) {
  const ra_theme_t *t = ra_theme_cur();
  lv_style_set_bg_color(&ra_st_screen, C(t->bg));
  lv_style_set_bg_opa(&ra_st_screen, LV_OPA_COVER);
  pad(&ra_st_screen, 0, 0);

  lv_style_set_bg_color(&ra_st_bar, C(t->bg));
  lv_style_set_bg_opa(&ra_st_bar, LV_OPA_COVER);
  lv_style_set_border_width(&ra_st_bar, 1);
  lv_style_set_border_side(&ra_st_bar, LV_BORDER_SIDE_BOTTOM);
  lv_style_set_border_color(&ra_st_bar, C(t->s2));
  pad(&ra_st_bar, 0, 0);
  lv_style_set_radius(&ra_st_bar, 0);

  lv_style_set_bg_color(&ra_st_btn, C(t->s2));
  lv_style_set_bg_opa(&ra_st_btn, LV_OPA_COVER);
  lv_style_set_radius(&ra_st_btn, RA_RADIUS_BTN);
  lv_style_set_border_width(&ra_st_btn, 0);
  pad(&ra_st_btn, 4, 4);

  lv_style_set_bg_color(&ra_st_card, C(t->s1));
  lv_style_set_bg_opa(&ra_st_card, LV_OPA_COVER);
  lv_style_set_radius(&ra_st_card, RA_RADIUS_CARD);
  lv_style_set_border_width(&ra_st_card, 0);
  pad(&ra_st_card, 4, 4);

  /* Pressed: lighter fill and a 2 px edge in TX, drawn inside the widget box (no outline, no growth). */
  lv_style_t *pr[2] = {&ra_st_btn_pr, &ra_st_card_pr};
  for (int i = 0; i < 2; i++) {
    lv_style_set_bg_color(pr[i], C(t->s3));
    lv_style_set_border_width(pr[i], 2);
    lv_style_set_border_color(pr[i], C(t->tx));
    lv_style_set_border_opa(pr[i], LV_OPA_COVER);
  }

  lv_style_set_bg_color(&ra_st_toast, C(t->bad));
  lv_style_set_bg_opa(&ra_st_toast, LV_OPA_COVER);
  lv_style_set_radius(&ra_st_toast, RA_RADIUS_CARD);
  lv_style_set_border_width(&ra_st_toast, 0);
  pad(&ra_st_toast, 0, 0);

  lv_style_set_bg_color(&ra_st_ink, C(t->tx2));
  lv_style_set_bg_opa(&ra_st_ink, LV_OPA_COVER);
  lv_style_set_border_width(&ra_st_ink, 0);
  lv_style_set_radius(&ra_st_ink, 1);
  pad(&ra_st_ink, 0, 0);

  txt(&ra_st_txt_title, &ra_font_b18, t->tx, LV_TEXT_ALIGN_LEFT);
  txt(&ra_st_txt_display, &ra_font_b20, t->tx, LV_TEXT_ALIGN_CENTER);
  txt(&ra_st_txt_body, &ra_font_m16, t->tx, LV_TEXT_ALIGN_CENTER);
  txt(&ra_st_txt_btn, &ra_font_b13, t->tx, LV_TEXT_ALIGN_CENTER);
  txt(&ra_st_txt_cap, &ra_font_r12, t->tx2, LV_TEXT_ALIGN_CENTER);
  txt(&ra_st_txt_ontoast, &ra_font_m16, t->onbad, LV_TEXT_ALIGN_LEFT);
}

/* The layers above the screens are not walked by lv_obj_report_style_change. */
static void refresh_tree(lv_obj_t *o) {
  lv_obj_refresh_style(o, LV_PART_ANY, LV_STYLE_PROP_ANY);
  for (uint32_t i = 0, n = lv_obj_get_child_count(o); i < n; i++) refresh_tree(lv_obj_get_child(o, (int32_t)i));
}

static void refresh_all(void) {
  build();
  if (!ready) return;
  lv_obj_report_style_change(NULL);
  refresh_tree(lv_layer_top());
  if (listener) listener(listener_ctx);
}

void ra_theme_init(void) {
  cur = ra_themes[RA_THEME_DEFAULT];
  cur_init = 1;
  lv_style_t *all[] = {&ra_st_screen, &ra_st_bar, &ra_st_btn, &ra_st_btn_pr, &ra_st_card, &ra_st_card_pr,
                       &ra_st_toast, &ra_st_txt_title, &ra_st_txt_display, &ra_st_txt_body, &ra_st_txt_btn,
                       &ra_st_txt_cap, &ra_st_txt_ontoast, &ra_st_ink};
  for (unsigned i = 0; i < sizeof all / sizeof *all; i++) lv_style_init(all[i]); /* the old pool is gone: just zero */
  ready = 0;
  const char *e = getenv("HARMONY_THEME"); /* bench aid: a preset key ("daylight") or a single-digit id */
  if (e && *e)
    for (unsigned i = 0; i < RA_THEME_COUNT; i++)
      if (!strcmp(e, ra_themes[i].key) || (e[0] >= '0' && e[0] <= '9' && !e[1] && (unsigned)(e[0] - '0') == ra_themes[i].id))
        cur = ra_themes[i];
  build();
  ready = 1;
}

int ra_theme_apply_preset(uint8_t id) {
  const ra_theme_t *p = ra_theme_find(id);
  if (!p) return -1;
  cur = *p;
  refresh_all();
  return 0;
}

int ra_theme_apply_wire(const uint8_t *bytes, size_t n) {
  ra_theme_t t;
  if (!bytes || n > 0xFFFF) return -1;
  int rc = ra_theme_decode(bytes, (unsigned)n, &t);
  if (rc < 0) return rc;
  cur = t;
  refresh_all();
  return 0;
}

void ra_theme_set_listener(void (*cb)(void *), void *ctx) {
  listener = cb;
  listener_ctx = ctx;
}

const ra_swatch_t *ra_theme_palette(unsigned idx) {
  return idx >= 1 && idx <= RA_PALETTE_N ? &ra_theme_cur()->palette[idx - 1] : NULL;
}

void ra_theme_free_colour(uint16_t rgb565, ra_swatch_t *out, int *outline) {
  const ra_theme_t *t = ra_theme_cur();
  out->fill = ra_from565(rgb565);
  out->ink = ra_ink(out->fill, t->tx, t->bg);
  out->pressed = ra_pressed(out->fill, out->ink);
  if (outline) *outline = ra_needs_outline(out->fill, t->bg);
}

void ra_theme_local(lv_obj_t *o, lv_obj_t *label, const ra_swatch_t *s, int outline) {
  const ra_theme_t *t = ra_theme_cur();
  lv_obj_set_style_bg_color(o, C(s->fill), 0);
  lv_obj_set_style_bg_color(o, C(s->pressed), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(o, outline ? 1 : 0, 0);
  lv_obj_set_style_border_color(o, C(t->tx3), 0);
  if (label) lv_obj_set_style_text_color(label, C(s->ink), 0);
}

/* Brand logo colours, icons 65..91 (harmony_core/icons.py BRAND_COLORS); 0 for any other icon. */
static const uint32_t brand_tab[] = {0xE5A00D, 0xFF0000, 0xE50914, 0x00A8E1, 0x113CCF, 0x002BE7, 0x1CE783, 0x000000, 0x0064FF, 0x000000, 0xF47521, 0x9146FF, 0x00A4DC, 0x52B54B, 0x17B2E7, 0x282A2D, 0x1DB954, 0xFA243C, 0x000000, 0xFF5500, 0x171A21, 0x107C10, 0x003791, 0xE60012, 0x76B900, 0x000000, 0x4285F4};
uint32_t ra_brand_rgb(uint8_t icon) {
  unsigned k = (unsigned)icon - 65u; /* the logos are one contiguous run of ids */
  if (icon < 65 || k >= sizeof brand_tab / sizeof brand_tab[0]) return 0;
  uint32_t c = brand_tab[k];
  return c ? c : 0x000001; /* black is a colour too: callers drop it as too dark */
}
