#include "remote/app/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hub/hp_errors.h"

#define TH (ra_theme_cur())

#define ELLIPSIS "\xe2\x80\xa6"

static void paint(ra_ui_t *ui, unsigned i);
static const char *tile_caption(const ra_ui_t *ui, const hp_widget_t *w);

static void stale_caption(ra_ui_t *ui, unsigned i);

static int edge(int i, int n, int total) { return (i * total + n - 1) / n; }

void ra_ui_widget_box(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2) {
  *x1 = edge(w->x, p->cols, HP_PAGE_SCREEN_W);
  *x2 = edge(w->x + w->w, p->cols, HP_PAGE_SCREEN_W) - 1;
  int sh = hp_page_row_shift(p), rows = p->rows - sh;
  *y1 = HP_PAGE_BAR_H + edge(w->y - sh, rows, HP_PAGE_CONTENT_H);
  *y2 = HP_PAGE_BAR_H + edge(w->y + w->h - sh, rows, HP_PAGE_CONTENT_H) - 1;
}

/* Inset one axis [lo,hi]: `a` at the low edge, `b` at the high edge, relaxed for tiny cells so a
 * widget never shrinks below ~12 px (or to nothing). */
static void inset(int *lo, int *hi, int a, int b) {
  int n = *hi - *lo + 1;
  while (n - a - b < 12 && (a || b)) {
    if (a) a--;
    if (b) b--;
  }
  *lo += a;
  *hi -= b;
}

void ra_ui_widget_rect(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2) {
  ra_ui_widget_box(p, w, x1, y1, x2, y2);
  inset(x1, x2, *x1 == 0 ? RA_MARGIN : RA_GAP / 2, *x2 == HP_PAGE_SCREEN_W - 1 ? RA_MARGIN : RA_GAP / 2);
  inset(y1, y2, *y1 == HP_PAGE_BAR_H ? RA_MARGIN : RA_GAP / 2, *y2 == HP_PAGE_SCREEN_H - 1 ? RA_MARGIN : RA_GAP / 2);
}

static void set_hidden(lv_obj_t *o, int hide) {
  if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Test hooks (host tests only): ra_ui_test_fail_at >= 0 makes the Nth object creation from now fail as if LVGL
 * had run out of memory; ra_ui_test_skip_precheck disables the free-memory precheck. */
int ra_ui_test_fail_at = -1;
int ra_ui_test_skip_precheck;
static int creation_fails(void) {
  if (ra_ui_test_fail_at < 0) return 0;
  return ra_ui_test_fail_at-- == 0;
}

static void plain(lv_obj_t *o) {
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* One-line label that ends in "..." instead of wrapping. */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_style_t *st, const lv_font_t *font, int w) {
  lv_obj_t *l = creation_fails() ? NULL : lv_label_create(parent);
  if (!l) return NULL;
  lv_obj_add_style(l, st, 0);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_size(l, w, lv_font_get_line_height(font ? font : lv_obj_get_style_text_font(l, 0)));
  lv_label_set_text(l, text);
  return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, const lv_style_t *st) {
  lv_obj_t *o = creation_fails() ? NULL : lv_obj_create(parent);
  if (!o) return NULL;
  plain(o);
  lv_obj_add_style(o, st, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  return o;
}

/* ---- status bar ---- */

#define BAT_X 208
#define BAT_Y 10
#define PCT_X 164
#define PCT_W 40
#define RF_X 146
#define TITLE_X 12
#define TITLE_W 128

static void chrome_apply(ra_ui_t *ui) {
  if (!ui->bar || !ui->title || !ui->rf[0] || !ui->rf[1] || !ui->rf[2] || !ui->pct || !ui->batt || !ui->batt_fill) return;
  lv_color_t rf = ra_color(ui->link_lost ? TH->bad : TH->tx2);
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(ui->rf[i], rf, 0);
    if (i) set_hidden(ui->rf[i], ui->link_lost); /* lost: one short red bar */
  }
  int low = ui->soc >= 0 && ui->soc <= RA_LOWBATT_PCT && !ui->charging;
  lv_color_t c = ra_color(low ? TH->bad : ui->charging ? TH->acc : TH->tx2);
  lv_obj_set_style_border_color(ui->batt, c, 0);
  lv_obj_set_style_bg_color(ui->batt_fill, c, 0);
  lv_obj_set_style_text_color(ui->pct, ra_color(low ? TH->bad : TH->tx2), 0);
  set_hidden(ui->batt_fill, ui->soc < 0);
  set_hidden(ui->pct, ui->soc < 0);
  if (ui->soc >= 0) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", ui->soc > 100 ? 100 : ui->soc);
    lv_label_set_text(ui->pct, b);
    int fw = (ui->soc * 18 + 50) / 100;
    lv_obj_set_width(ui->batt_fill, fw < 1 ? 1 : fw);
  }
}

#define SEQ_RIGHT 142 /* right edge of the "idx/cnt" label, left of the RF bars */
#define BACK_W 24     /* back chevron cell; a tap on x < BACK_TAP_W goes Home (app.c) */

/* The bar's page context: a back chevron (BACK pages) and "idx/cnt" (paged groups). */
typedef struct {
  int back;
  unsigned seq_idx, seq_cnt;
} bar_ctx_t;

/* 0 ok, -1 out of memory (the caller shows the error state; chrome_apply ignores a partial bar). */
static int bar_build(ra_ui_t *ui, lv_obj_t *scr, const char *title, const bar_ctx_t *bc) {
  ui->bar = ui->title = ui->pct = ui->batt = ui->batt_fill = ui->back = ui->seq = NULL;
  ui->rf[0] = ui->rf[1] = ui->rf[2] = NULL;
  lv_obj_t *bar = rect(scr, 0, 0, HP_PAGE_SCREEN_W, RA_BAR_H, &ra_st_bar);
  if (!bar) return -1;
  int tx = TITLE_X, tw = TITLE_W;
  if (bc && bc->back) {
    tx += BACK_W + 2;
    tw -= BACK_W + 2;
    lv_obj_t *b = make_label(bar, RA_GLYPH_BACK, &ra_st_txt_cap, &ra_icons_24, BACK_W);
    if (!b) return -1;
    lv_obj_set_pos(b, 4, 4);
    ui->back = b;
  }
  if (bc && bc->seq_cnt) {
    char sq[24];
    snprintf(sq, sizeof sq, "%u/%u", bc->seq_idx, bc->seq_cnt);
    lv_obj_t *q = make_label(bar, sq, &ra_st_txt_cap, NULL, 40);
    if (!q) return -1;
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(q, ra_color(TH->tx3), 0);
    lv_obj_set_pos(q, SEQ_RIGHT - 40, 9);
    ui->seq = q;
    tw = SEQ_RIGHT - 40 - 4 - tx;
  }
  lv_obj_t *t = make_label(bar, title, &ra_st_txt_title, NULL, tw);
  if (!t) return -1;
  lv_obj_set_pos(t, tx, 6);
  lv_obj_t *rf[3];
  for (int i = 0; i < 3; i++) {
    int h = 4 + 4 * i;
    rf[i] = rect(bar, RF_X + 5 * i, 22 - h, 3, h, &ra_st_ink);
    if (!rf[i]) return -1;
  }
  lv_obj_t *pct = make_label(bar, "", &ra_st_txt_cap, NULL, PCT_W);
  if (!pct) return -1;
  lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(pct, PCT_X, 7);
  lv_obj_t *batt = rect(bar, BAT_X, BAT_Y, 22, 12, &ra_st_ink);
  if (!batt) return -1;
  lv_obj_set_style_bg_opa(batt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(batt, 1, 0);
  lv_obj_set_style_radius(batt, 2, 0);
  lv_obj_t *fill = rect(batt, 1, 1, 18, 8, &ra_st_ink);
  if (!fill || !rect(bar, BAT_X + 23, BAT_Y + 3, 2, 6, &ra_st_ink)) return -1;
  ui->title = t;
  ui->rf[0] = rf[0];
  ui->rf[1] = rf[1];
  ui->rf[2] = rf[2];
  ui->pct = pct;
  ui->batt = batt;
  ui->batt_fill = fill;
  ui->bar = bar;
  chrome_apply(ui);
  return 0;
}

void ra_ui_set_link(ra_ui_t *ui, int lost) {
  lost = lost != 0;
  if (ui->link_lost == lost) return;
  ui->link_lost = lost;
  chrome_apply(ui);
  if (ui->page.version != 2) return;
  for (unsigned i = 0; i < ui->page.count; i++) { /* v2 tiles say "stale" while the link is lost */
    if (ui->page.widgets[i].kind != HP_WIDGET_TILE || !ui->obj[i]) continue;
    if (!ui->wcap[i] && lost) stale_caption(ui, i); /* a tile without a name gets its caption now (optional: skipped when memory is tight) */
    if (ui->wcap[i]) lv_label_set_text(ui->wcap[i], tile_caption(ui, &ui->page.widgets[i]));
    paint(ui, i);
  }
}

void ra_ui_set_battery(ra_ui_t *ui, int soc, int charging) {
  charging = charging != 0;
  if (ui->soc == soc && ui->charging == charging) return;
  ui->soc = soc;
  ui->charging = charging;
  chrome_apply(ui);
}

/* ---- pages ---- */

/* Memory ran out: drop whatever was built and show the minimal error state. Its objects (ui->err) were created
 * once at init, so showing it allocates nothing. The page on screen is forgotten (no taps hit invisible widgets). */
static void fail(ra_ui_t *ui) {
  lv_obj_clean(lv_screen_active());
  memset(ui->obj, 0, sizeof ui->obj);
  memset(ui->lbl, 0, sizeof ui->lbl);
  memset(ui->ico, 0, sizeof ui->ico);
  memset(ui->wcap, 0, sizeof ui->wcap);
  ui->active = 0;
  ui->msg = ui->cap = ui->prog_fill = ui->bar = ui->title = ui->pct = ui->batt = ui->batt_fill = ui->back = ui->seq = NULL;
  ui->recv_total = 0;
  ui->rf[0] = ui->rf[1] = ui->rf[2] = NULL;
  ui->page.count = 0;
  ui->page.page_id = RA_UI_PAGE_ERROR;
  ui->title_idx = ui->pressed = -1;
  ui->fallback = 0;
  ui->oom++;
  if (ui->err) lv_obj_remove_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
}

/* Free memory needed before building a page, an upper bound of what test_remote_mem measures: <= 0.94 KiB per widget
 * (32 buttons with an icon and a label: 30 KiB above the 9 KiB idle use, i.e. 0.94 KiB each with the icon; budgeted 0.9
 * KiB for surface + text plus 0.25 KiB per extra object), plus the bar and the renderer's working set. `extra` counts only
 * objects that are really created (an icon glyph; a tile's icon only when its cell is big enough, and its caption), so a
 * legal page is never refused for objects it will not draw. Refusing early is what keeps LVGL's own allocations (which do
 * not check for failure) safe. */
#define BUILD_NEED(widgets, extra) (900u * (widgets) + 250u * (extra) + 8192u)

static int enough_memory(unsigned need) {
  if (ra_ui_test_skip_precheck) return 1;
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  return mon.free_size >= need;
}

static int screen_reset(ra_ui_t *ui, const char *title, unsigned widgets, unsigned extra, const bar_ctx_t *bc) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_remove_style_all(scr);
  lv_obj_add_style(scr, &ra_st_screen, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  if (ui->err) lv_obj_add_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
  memset(ui->obj, 0, sizeof ui->obj);
  memset(ui->lbl, 0, sizeof ui->lbl);
  memset(ui->ico, 0, sizeof ui->ico);
  memset(ui->wcap, 0, sizeof ui->wcap);
  ui->active = 0;
  ui->msg = ui->cap = ui->prog_fill = NULL;
  ui->recv_total = 0;
  ui->title_idx = -1;
  ui->pressed = -1;
  if (!enough_memory(BUILD_NEED(widgets, extra)) || bar_build(ui, scr, title, bc) < 0) {
    fail(ui);
    return -1;
  }
  return 0;
}

/* Centred two-line state message in the content area; 0 ok, -1 out of memory. */
static int message(ra_ui_t *ui, const char *head, const char *sub, int head_bad) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *m = make_label(scr, head, &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
  if (!m) return -1;
  ui->msg = m;
  ui->msg_bad = head_bad;
  lv_obj_set_style_text_color(m, ra_color(head_bad ? TH->bad : TH->tx), 0);
  lv_obj_set_pos(m, RA_MARGIN, 148);
  lv_obj_t *c = make_label(scr, sub, &ra_st_txt_cap, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
  if (!c) return -1;
  ui->cap = c;
  lv_obj_set_pos(c, RA_MARGIN, 176);
  return 0;
}

/* ---- v2 widgets: colours by role / state, icon, shape ---- */

typedef struct {
  uint32_t fill, ink, icon, cap;
  uint32_t pressed;
  uint8_t local;   /* fill / pressed / outline come from a swatch: set them as local styles */
  uint8_t outline; /* 1 px TX3 outline (the fill is too close to the theme ground) */
  uint8_t border;  /* 1: a 3 px active border in border_rgb */
  uint32_t border_rgb;
} wcol_t;

#define WC(f, i, ic, cp) ((wcol_t){.fill = (f), .ink = (i), .icon = (ic), .cap = (cp)})

static int is_stale_tile(const ra_ui_t *ui, const hp_widget_t *w) { return w->kind == HP_WIDGET_TILE && ui->link_lost; }

/* A widget colour in the current theme: a palette entry or a free RGB565. 0 = none / not resolvable. */
static int colour_swatch(hp_color_t c, ra_swatch_t *s, int *outline) {
  if (c.kind == HP_COLOR_PALETTE) {
    const ra_swatch_t *p = ra_theme_palette(c.value);
    if (!p) return 0;
    *s = *p;
    *outline = ra_needs_outline(p->fill, TH->bg);
    return 1;
  }
  if (c.kind == HP_COLOR_RGB) {
    ra_theme_free_colour((uint16_t)c.value, s, outline);
    return 1;
  }
  return 0;
}

static wcol_t from_swatch(const ra_swatch_t *s, int outline) {
  return (wcol_t){s->fill, s->ink, s->ink, s->ink, s->pressed, 1, (uint8_t)outline, 0, 0};
}

static wcol_t widget_colors(const ra_ui_t *ui, const hp_widget_t *w, int active) {
  ra_swatch_t sw;
  int ol = 0;
  if (w->kind == HP_WIDGET_TILE) {
    wcol_t c;
    int stale = is_stale_tile(ui, w);
    switch (w->tstate) {
    case HP_TILE_ON:
      if (colour_swatch(w->color_on, &sw, &ol)) return from_swatch(&sw, ol);
      c = WC(TH->warm, TH->onwarm, TH->onwarm, TH->onwarm);
      break;
    case HP_TILE_UNAVAILABLE: c = WC(TH->s1, TH->tx3, TH->tx3, TH->tx3); break;
    case HP_TILE_OFF: c = WC(TH->s1, TH->tx2, TH->tx3, TH->tx3); break;
    default: c = WC(TH->s1, TH->tx, TH->tx2, TH->tx2); break;
    }
    /* state wins: stale / unavailable keep the theme's state colours; a set fill recolours off / value only */
    if (!stale && w->tstate != HP_TILE_ON && w->tstate != HP_TILE_UNAVAILABLE && colour_swatch(w->color_fill, &sw, &ol))
      return from_swatch(&sw, ol);
    if (stale && w->tstate != HP_TILE_ON) c.ink = TH->tx3;
    return c;
  }
  if (w->kind == HP_WIDGET_BUTTON && colour_swatch(w->color_fill, &sw, &ol)) {
    wcol_t c = from_swatch(&sw, ol);
    if (active) { /* the highlight keeps the fill: a 3 px ACC border (TX when the fill is the accent) */
      c.border = 1;
      c.border_rgb = sw.fill == TH->acc ? TH->tx : TH->acc;
    }
    return c;
  }
  if (active) return WC(TH->acc, TH->onacc, TH->onacc, TH->onacc);
  switch (w->role) {
  case HP_ROLE_PRIMARY: return WC(TH->s2, TH->acc, TH->acc, TH->tx2);
  case HP_ROLE_SECONDARY: return WC(TH->s1, TH->tx2, TH->tx3, TH->tx3);
  case HP_ROLE_DANGER: return WC(TH->s2, TH->bad, TH->bad, TH->bad);
  case HP_ROLE_NAV: return WC(TH->s1, TH->tx, TH->acc, TH->tx2);
  default: return WC(TH->s2, TH->tx, TH->tx2, TH->tx2);
  }
}

static int widget_active(const ra_ui_t *ui, const hp_widget_t *w) {
  return ui->act_on && w->kind == HP_WIDGET_BUTTON && w->role != HP_ROLE_NAV && w->has_bind && w->bind == ui->act_idx;
}

/* The text a tile shows: "n/a" while unavailable. */
static const char *tile_value(const hp_widget_t *w) { return w->tstate == HP_TILE_UNAVAILABLE ? "n/a" : w->text; }

/* Caption of a v2 tile: "stale" while the link is lost, else its name. */
static const char *tile_caption(const ra_ui_t *ui, const hp_widget_t *w) { return ui->link_lost ? "stale" : w->label; }

/* (Re)paints widget i from its role, state and the activity / link state: fill and ink colours only, so only that
 * widget's area is invalidated. */
static void paint(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (!ui->obj[i]) return;
  if (w->kind == HP_WIDGET_LABEL) { /* a coloured section label: its colour when readable on the ground, else TX */
    ra_swatch_t sw;
    int ol;
    if (ui->lbl[i] && colour_swatch(w->color_fill, &sw, &ol))
      lv_obj_set_style_text_color(ui->lbl[i], ra_color(ra_meets(sw.fill, TH->bg, 9, 2) ? sw.fill : TH->tx), 0);
    return;
  }
  int act = widget_active(ui, w);
  wcol_t c = widget_colors(ui, w, act);
  lv_obj_t *o = ui->obj[i];
  lv_obj_set_style_bg_color(o, ra_color(c.fill), 0);
  if (c.local) {
    ra_swatch_t sw = {c.fill, c.ink, c.pressed};
    ra_theme_local(o, NULL, &sw, c.outline);
    if (c.border) {
      lv_obj_set_style_border_width(o, 3, 0);
      lv_obj_set_style_border_color(o, ra_color(c.border_rgb), 0);
    }
  } else if (w->color_fill.kind || w->color_on.kind) { /* coloured elsewhere (another tile state): the shared look */
    lv_obj_remove_local_style_prop(o, LV_STYLE_BG_COLOR, LV_STATE_PRESSED);
    lv_obj_remove_local_style_prop(o, LV_STYLE_BORDER_WIDTH, 0);
    lv_obj_remove_local_style_prop(o, LV_STYLE_BORDER_COLOR, 0);
  }
  if (ui->lbl[i]) lv_obj_set_style_text_color(ui->lbl[i], ra_color(c.ink), 0);
  if (ui->ico[i]) lv_obj_set_style_text_color(ui->ico[i], ra_color(c.icon), 0);
  if (ui->wcap[i]) lv_obj_set_style_text_color(ui->wcap[i], ra_color(c.cap), 0);
  if (act) ui->active |= 1u << i;
  else ui->active &= ~(1u << i);
}

static const lv_font_t *icon_font(int px) { return px >= 32 ? &ra_icons_32 : px >= 24 ? &ra_icons_24 : &ra_icons_20; }

/* UTF-8 of the icon-font glyph U+E000 + id; NULL for "no icon" and for ids this font does not know. */
static const char *glyph(uint8_t id, char buf[4]) {
  if (!id || id > RA_ICON_MAX) return NULL;
  unsigned cp = RA_ICON_GLYPH_BASE + id;
  buf[0] = (char)(0xE0 | (cp >> 12));
  buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  buf[2] = (char)(0x80 | (cp & 0x3F));
  buf[3] = 0;
  return buf;
}

static size_t first_char_len(const char *s) {
  unsigned char c = (unsigned char)s[0];
  size_t k = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
  size_t n = 0;
  while (n < k && s[n]) n++;
  return n;
}

/* Builds the content of a v2 button / tile on its surface `o`. 0 ok, -1 out of memory. */
static int build_v2_content(ra_ui_t *ui, unsigned i, lv_obj_t *o, int bw, int bh) {
  const hp_widget_t *w = &ui->page.widgets[i];
  int tile = w->kind == HP_WIDGET_TILE;
  char gb[4];
  const char *g = glyph(w->icon, gb);
  int draw_label = !(w->icon_only && g) && w->label[0];
  lv_obj_t *l = NULL, *ic = NULL;
  if (tile) {
    int big = bw >= 80;
    int cap_h = bh >= 44 && (w->label[0] || ui->link_lost);
    if (g && bh >= 56 && bw >= 56) {
      ic = make_label(o, g, &ra_st_txt_cap, &ra_icons_20, 24);
      if (!ic) return -1;
      lv_obj_set_pos(ic, 8, 6);
    }
    l = make_label(o, tile_value(w), big ? &ra_st_txt_display : &ra_st_txt_btn, NULL, bw - 8);
    if (!l) return -1;
    lv_obj_align(l, LV_ALIGN_CENTER, 0, cap_h ? 6 : 0);
    if (cap_h) {
      lv_obj_t *c = make_label(o, tile_caption(ui, w), &ra_st_txt_cap, NULL, bw - (ic ? 38 : 8));
      if (!c) return -1;
      if (ic) {
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(c, 32, 8);
      }
      else lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 6);
      ui->wcap[i] = c;
    }
    ui->lbl[i] = l;
    ui->ico[i] = ic;
    return 0;
  }
  const char *txt = w->label;
  char mono[8];
  if (w->shape == HP_SHAPE_MONOGRAM) { /* a round badge: the icon, else the label's first letter */
    draw_label = !g && w->label[0];
    size_t k = first_char_len(w->label);
    memcpy(mono, w->label, k);
    mono[k] = 0;
    if (k == 1) mono[0] = (char)toupper((unsigned char)mono[0]);
    txt = mono;
  }
  int row = w->shape == HP_SHAPE_ROW || w->shape == HP_SHAPE_PILL || (bh < 44 && bw >= 100); /* wide and short: a row */
  int big = w->shape == HP_SHAPE_CARD && !row && bw >= 100 && bh >= 80;
  int mono_px = bw < bh ? bw : bh;
  int px = w->shape == HP_SHAPE_MONOGRAM ? (mono_px >= 72 ? 32 : mono_px >= 44 ? 24 : 20)
           : row ? 24 : big ? 32 : bh >= 64 ? 24 : 20;
  if (g) {
    ic = make_label(o, g, &ra_st_txt_cap, icon_font(px), px + 4);
    if (!ic) return -1;
  }
  if (draw_label) {
    const lv_style_t *ts = w->shape == HP_SHAPE_MONOGRAM ? &ra_st_txt_display : big ? &ra_st_txt_display
                           : row ? &ra_st_txt_body : (bw >= 100 ? &ra_st_txt_body : &ra_st_txt_btn);
    const lv_font_t *tf = big ? &ra_font_b18 : NULL;
    int lw = big ? bw - 24 : bw - 8;
    if (row && ic) lw = bw - 12 - px - 8 - 8;
    else if (row) lw = bw - 24;
    if (row && w->trailing) lw -= 24 + 4; /* room for the chevron */
    if (lw < 0) lw = 0;
    l = make_label(o, txt, ts, tf, lw);
    if (!l) return -1;
  }
  /* placement */
  if (ic && (w->icon_only || w->shape == HP_SHAPE_MONOGRAM || !draw_label)) {
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
    if (l) { lv_obj_delete(l); l = NULL; }
  } else if (row) {
    if (w->trailing) {
      lv_obj_t *ch = make_label(o, RA_GLYPH_CHEV_RIGHT, &ra_st_txt_cap, icon_font(24), 28);
      if (!ch) return -1;
      lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    if (ic) lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    if (l) {
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(l, LV_ALIGN_LEFT_MID, ic ? 12 + px + 8 : 12, 0);
    }
  } else if (big) {
    if (ic) lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 12, 12);
    if (l) {
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    }
  } else if (ic && l) { /* dense: icon over label, both centred */
    int ih = lv_font_get_line_height(icon_font(px)), lh = lv_font_get_line_height(lv_obj_get_style_text_font(l, 0));
    int gap = (bh - ih - lh) / 3;
    if (gap < 0) gap = 0;
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, gap + 1);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -gap);
  } else if (l) {
    lv_obj_center(l);
  }
  ui->lbl[i] = l;
  ui->ico[i] = ic;
  return 0;
}

static void build(ra_ui_t *ui) {
  lv_obj_t *scr = lv_screen_active();
  const hp_page_t *p = &ui->page;
  int v2 = p->version == 2;
  const char *title = "Harmony";
  int title_idx = v2 ? -1 : hp_page_title(p);
  if (v2) {
    if (p->title[0]) title = p->title;
  } else if (title_idx >= 0 && p->widgets[title_idx].label[0]) {
    title = p->widgets[title_idx].label;
  }
  unsigned extra = 0; /* icon glyph labels and tile captions */
  for (unsigned i = 0; v2 && i < p->count; i++) {
    const hp_widget_t *w = &p->widgets[i];
    int x1, y1, x2, y2;
    ra_ui_widget_rect(p, w, &x1, &y1, &x2, &y2);
    int bw = x2 - x1 + 1, bh = y2 - y1 + 1, icon = w->icon && w->icon <= RA_ICON_MAX;
    if (w->kind == HP_WIDGET_TILE) /* as build_v2_content: the icon needs a 56 px cell, the caption 44 px of height */
      extra += (icon && bh >= 56 && bw >= 56) + (bh >= 44 && (w->label[0] || ui->link_lost));
    else if (w->kind == HP_WIDGET_BUTTON) extra += icon;
    extra += 2 * (w->color_fill.kind || w->color_on.kind); /* local styles */
  }
  bar_ctx_t bc = {.back = v2 && p->back, .seq_idx = v2 ? p->seq_idx : 0, .seq_cnt = v2 ? p->seq_cnt : 0};
  if (screen_reset(ui, title, p->count, extra, &bc) < 0) return;
  ui->title_idx = title_idx;
  int interactive_or_text = 0;
  for (unsigned i = 0; i < p->count; i++) {
    const hp_widget_t *w = &p->widgets[i];
    if ((int)i == ui->title_idx) continue;
    interactive_or_text++;
    int x1, y1, x2, y2;
    ra_ui_widget_rect(p, w, &x1, &y1, &x2, &y2);
    int bw = x2 - x1 + 1, bh = y2 - y1 + 1;
    lv_obj_t *o, *l;
    if (w->kind == HP_WIDGET_LABEL) {
      /* A bare section label: no surface, clipped to its cells. */
      o = rect(scr, x1, y1, bw, bh, &ra_st_screen);
      if (!o) return fail(ui);
      lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
      l = make_label(o, w->label, &ra_st_txt_cap, &ra_font_m16, bw);
      if (!l) return fail(ui);
      lv_obj_center(l);
      ui->obj[i] = o;
      ui->lbl[i] = l;
      paint(ui, i);
      continue;
    }
    int btn = w->kind == HP_WIDGET_BUTTON;
    o = rect(scr, x1, y1, bw, bh, btn ? &ra_st_btn : &ra_st_card);
    if (!o) return fail(ui);
    lv_obj_add_style(o, btn ? &ra_st_btn_pr : &ra_st_card_pr, LV_STATE_PRESSED);
    ui->obj[i] = o;
    if (v2) {
      int r = w->shape == HP_SHAPE_MONOGRAM ? LV_RADIUS_CIRCLE : w->shape == HP_SHAPE_PILL ? bh / 2
              : w->shape == HP_SHAPE_ROW ? 12 : -1;
      if (r >= 0) lv_obj_set_style_radius(o, r, 0);
      if (build_v2_content(ui, i, o, bw, bh) < 0) return fail(ui);
      paint(ui, i);
      continue;
    }
    const char *txt = btn ? w->label : w->text;
    const lv_style_t *ts = btn ? (bw >= 100 ? &ra_st_txt_body : &ra_st_txt_btn) : (bw >= 80 ? &ra_st_txt_display : &ra_st_txt_btn);
    l = make_label(o, txt, ts, NULL, bw - 8);
    if (!l) return fail(ui);
    lv_obj_center(l);
    if (btn && !strcasecmp(w->label, "Power Off")) lv_obj_set_style_text_color(l, ra_color(TH->bad), 0);
    ui->lbl[i] = l;
  }
  if (!interactive_or_text && message(ui, "No activities yet", "Add some in Home Assistant", 0) < 0) fail(ui);
}

/* The custom theme slot (wire form), kept for pages with theme id 7. */
static uint8_t custom_wire[RA_WIRE_LEN];
static int custom_loaded;
static ra_ui_t *theme_ui;

static void apply_custom(void) {
  if (!custom_loaded || ra_theme_apply_wire(custom_wire, sizeof custom_wire) < 0) ra_theme_apply_preset(RA_THEME_DEFAULT);
}

int ra_ui_set_custom_theme(const uint8_t *bytes, size_t n) {
  ra_theme_t t;
  if (!bytes || n != RA_WIRE_LEN || ra_theme_decode(bytes, (unsigned)n, &t) < 0) return -1;
  memcpy(custom_wire, bytes, n);
  custom_loaded = 1;
  if (theme_ui && theme_ui->page_theme == HP_PAGE_THEME_CUSTOM) apply_custom();
  return 0;
}

/* A v2 page names its theme: apply it when it differs from the previous page's (cheap no-op otherwise). v1 pages
 * and the built-in screens leave the theme alone. */
static void page_theme(ra_ui_t *ui, const hp_page_t *p) {
  if (p->version != 2 || p->theme == ui->page_theme) return;
  ui->page_theme = p->theme;
  if (p->theme == HP_PAGE_THEME_CUSTOM) apply_custom();
  else if (ra_theme_apply_preset(p->theme) < 0) ra_theme_apply_preset(RA_THEME_DEFAULT);
}

void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p) {
  page_theme(ui, p);
  ui->page = *p;
  ui->fallback = 0;
  build(ui);
}

static void show_builtin(ra_ui_t *ui, uint8_t id, const char *head, const char *sub, int head_bad) {
  memset(&ui->page, 0, sizeof ui->page);
  ui->page.page_id = id;
  ui->page.cols = ui->page.rows = 1;
  if (screen_reset(ui, "Harmony", 0, 0, NULL) < 0) return;
  if (message(ui, head, sub, head_bad) < 0) fail(ui);
}

/* Theme switched: recolour what carries a local colour (the shared styles were already updated in place). */
static void retheme(void *ctx) {
  ra_ui_t *ui = ctx;
  chrome_apply(ui);
  if (ui->seq) lv_obj_set_style_text_color(ui->seq, ra_color(TH->tx3), 0);
  if (ui->msg) lv_obj_set_style_text_color(ui->msg, ra_color(ui->msg_bad ? TH->bad : TH->tx), 0);
  if (ui->err_msg) lv_obj_set_style_text_color(ui->err_msg, ra_color(TH->bad), 0);
  if (ui->prog_fill) lv_obj_set_style_bg_color(ui->prog_fill, ra_color(TH->acc), 0);
  if (ui->page.page_id >= RA_UI_PAGE_ERROR) return; /* built-in screens: no widgets */
  for (unsigned i = 0; i < ui->page.count; i++) {
    if (ui->page.version == 2) paint(ui, i);
    else if (ui->lbl[i] && ui->page.widgets[i].kind == HP_WIDGET_BUTTON && !strcasecmp(ui->page.widgets[i].label, "Power Off"))
      lv_obj_set_style_text_color(ui->lbl[i], ra_color(TH->bad), 0);
  }
}

void ra_ui_init(ra_ui_t *ui) {
  memset(ui, 0, sizeof *ui);
  theme_ui = ui;
  custom_loaded = 0; /* a restart has no custom theme until theme_push delivers one */
  ra_theme_init();
  ra_theme_set_listener(retheme, ui);
  ui->soc = -1;
  ui->title_idx = ui->pressed = -1;
  /* The error state, built once while the pool is empty and kept hidden on the top layer. */
  ui->err = rect(lv_layer_top(), 0, 0, HP_PAGE_SCREEN_W, HP_PAGE_SCREEN_H, &ra_st_screen);
  if (ui->err) {
    lv_obj_t *m = make_label(ui->err, "Display error", &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
    lv_obj_t *c = make_label(ui->err, "Waiting for the next page", &ra_st_txt_cap, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
    ui->err_msg = m;
    if (m) {
      lv_obj_set_style_text_color(m, ra_color(TH->bad), 0);
      lv_obj_set_pos(m, RA_MARGIN, 148);
    }
    if (c) lv_obj_set_pos(c, RA_MARGIN, 176);
    lv_obj_add_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
  }
  ra_ui_show_boot(ui);
}

void ra_ui_show_boot(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_BOOT, "Connecting" ELLIPSIS, "Looking for the hub", 0);
  ui->fallback = 0;
}

void ra_ui_show_receiving(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_RECEIVING, "Receiving config" ELLIPSIS, "Waiting for the first page", 0);
  ui->fallback = 0;
}

#define PROG_X RA_MARGIN
#define PROG_Y 208
#define PROG_W (HP_PAGE_SCREEN_W - 2 * RA_MARGIN)
#define PROG_H 6

static void recv_texts(ra_ui_t *ui, unsigned k, unsigned n) {
  char b[32];
  snprintf(b, sizeof b, "%u of %u", k, n);
  if (ui->cap) lv_label_set_text(ui->cap, b);
  if (ui->prog_fill) lv_obj_set_width(ui->prog_fill, n ? (int)((unsigned)PROG_W * (k > n ? n : k) / n) + (k ? 0 : 0) : 0);
  if (ui->prog_fill) set_hidden(ui->prog_fill, k == 0);
}

void ra_ui_show_receiving_n(ra_ui_t *ui, unsigned k, unsigned n) {
  if (!n) n = 1;
  if (ui->page.page_id == RA_UI_PAGE_RECEIVING && ui->recv_total && !ui->fallback && ui->msg) {
    ui->recv_total = (int)n;
    recv_texts(ui, k, n); /* same screen: only the numbers change */
    return;
  }
  show_builtin(ui, RA_UI_PAGE_RECEIVING, "Receiving page", "", 0);
  if (ui->page.page_id != RA_UI_PAGE_RECEIVING) return; /* the error state */
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *track = rect(scr, PROG_X, PROG_Y, PROG_W, PROG_H, &ra_st_btn);
  lv_obj_t *fill = track ? rect(track, 0, 0, 1, PROG_H, &ra_st_ink) : NULL;
  if (!track || !fill) return fail(ui);
  lv_obj_set_style_radius(track, 3, 0);
  lv_obj_set_style_radius(fill, 3, 0);
  lv_obj_set_style_bg_color(fill, ra_color(TH->acc), 0);
  ui->prog_fill = fill;
  ui->fallback = 0;
  ui->recv_total = (int)n;
  recv_texts(ui, k, n);
}

void ra_ui_show_fallback(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_FALLBACK, "Hub unreachable", "Reconnecting automatically", 1);
  ui->fallback = 1;
}

static void stale_caption(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  int x1, y1, x2, y2;
  ra_ui_widget_rect(&ui->page, w, &x1, &y1, &x2, &y2);
  int bw = x2 - x1 + 1, bh = y2 - y1 + 1;
  if (bh < 44 || !enough_memory(4096)) return;
  lv_obj_t *c = make_label(ui->obj[i], "stale", &ra_st_txt_cap, NULL, bw - (ui->ico[i] ? 38 : 8));
  if (!c) return;
  if (ui->ico[i]) {
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(c, 32, 8);
  } else {
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 6);
  }
  ui->wcap[i] = c;
  lv_obj_align(ui->lbl[i], LV_ALIGN_CENTER, 0, 6);
}

/* Live text / state of a tile on screen: the label text and colours only. */
static void tile_refresh(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (ui->lbl[i]) lv_label_set_text(ui->lbl[i], ui->page.version == 2 ? tile_value(w) : w->text);
  if (ui->page.version == 2) paint(ui, i);
}

int ra_ui_tile_text(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, const char *text, size_t len) {
  if (page_id != ui->page.page_id) return -HP_E_NOENT;
  int r = hp_page_set_tile_text(&ui->page, tile_id, text, len);
  if (r < 0) return r;
  for (unsigned i = 0; i < ui->page.count; i++)
    if (ui->page.widgets[i].id == tile_id && ui->page.widgets[i].kind == HP_WIDGET_TILE && ui->lbl[i]) tile_refresh(ui, i);
  return 0;
}

int ra_ui_tile_state(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, uint8_t state, const char *text, size_t len) {
  if (page_id != ui->page.page_id) return -HP_E_NOENT;
  int r = hp_page_set_tile_text(&ui->page, tile_id, text, len);
  if (r < 0) return r;
  for (unsigned i = 0; i < ui->page.count; i++) {
    hp_widget_t *w = &ui->page.widgets[i];
    if (w->id == tile_id && w->kind == HP_WIDGET_TILE) {
      w->tstate = state;
      if (ui->lbl[i]) tile_refresh(ui, i);
    }
  }
  return 0;
}

void ra_ui_set_activity(ra_ui_t *ui, uint8_t activity, int power) {
  power = power != 0;
  if (ui->act_idx == activity && ui->act_on == power) return;
  ui->act_idx = activity;
  ui->act_on = power;
  if (ui->page.version != 2) return;
  for (unsigned i = 0; i < ui->page.count; i++) {
    const hp_widget_t *w = &ui->page.widgets[i];
    if (!ui->obj[i] || w->kind != HP_WIDGET_BUTTON) continue;
    int now = widget_active(ui, w), was = (ui->active >> i) & 1;
    if (now != was) paint(ui, i);
  }
}

/* ---- pressed feedback ---- */

static void set_pressed(ra_ui_t *ui, int idx) {
  if (idx == ui->pressed) return;
  if (ui->pressed >= 0 && ui->obj[ui->pressed]) lv_obj_remove_state(ui->obj[ui->pressed], LV_STATE_PRESSED);
  ui->pressed = idx;
  if (idx >= 0 && ui->obj[idx]) lv_obj_add_state(ui->obj[idx], LV_STATE_PRESSED);
}

void ra_ui_press_at(ra_ui_t *ui, int x, int y) {
  const hp_widget_t *w = hp_page_hit(&ui->page, x, y);
  set_pressed(ui, w && w->kind != HP_WIDGET_LABEL ? (int)(w - ui->page.widgets) : -1);
}

void ra_ui_release(ra_ui_t *ui) { set_pressed(ui, -1); }

/* ---- toast ---- */

static void toast_timeout(lv_timer_t *t) {
  ra_ui_t *ui = lv_timer_get_user_data(t);
  ui->toast_timer = NULL; /* one-shot: LVGL deletes it after this callback */
  ra_ui_toast_dismiss(ui);
}

void ra_ui_toast_dismiss(ra_ui_t *ui) {
  if (ui->toast_timer) {
    lv_timer_delete(ui->toast_timer);
    ui->toast_timer = NULL;
  }
  if (ui->toast) {
    lv_obj_delete(ui->toast);
    ui->toast = NULL;
  }
}

void ra_ui_toast(ra_ui_t *ui, const char *line1, const char *line2) {
  ra_ui_toast_dismiss(ui);
  if (!enough_memory(4096)) return; /* a toast is optional: skip it rather than risk the pool */
  int w = HP_PAGE_SCREEN_W - 2 * RA_MARGIN;
  lv_obj_t *t = rect(lv_layer_top(), RA_MARGIN, HP_PAGE_SCREEN_H - RA_MARGIN - RA_TOAST_H, w, RA_TOAST_H, &ra_st_toast);
  if (!t) return;
  lv_obj_t *l1 = make_label(t, line1, &ra_st_txt_ontoast, NULL, w - 24);
  lv_obj_t *l2 = line2 ? make_label(t, line2, &ra_st_txt_ontoast, &ra_font_r12, w - 24) : NULL;
  if (!l1 || (line2 && !l2)) {
    lv_obj_delete(t);
    return;
  }
  lv_obj_set_pos(l1, 12, line2 ? 8 : 18);
  if (l2) lv_obj_set_pos(l2, 12, 32);
  ui->toast_timer = lv_timer_create(toast_timeout, RA_TOAST_MS, ui);
  if (!ui->toast_timer) {
    lv_obj_delete(t);
    return;
  }
  lv_timer_set_repeat_count(ui->toast_timer, 1);
  ui->toast = t;
}
