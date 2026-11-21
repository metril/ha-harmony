#include "remote/app/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hub/hp_errors.h"
#include "remote/app/cover.h"
#include "remote/page/store.h"

#define TH (ra_theme_cur())

#define ELLIPSIS "\xe2\x80\xa6"

static void paint(ra_ui_t *ui, unsigned i);
static void np_forget(ra_ui_t *ui);
static const char *tile_state_text(const ra_ui_t *ui, const hp_widget_t *w);

void ra_ui_widget_box(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2) {
  if (hp_page_box(p, w, x1, y1, x2, y2)) *x1 = *y1 = *x2 = *y2 = 0;
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
  if (p->fixed) return; /* FIXED boxes are already the drawn rectangle: gaps are between widgets */
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
#define BACK_W 24     /* back chevron cell; a tap on x < BACK_TAP_W goes back (app.c) */

/* The bar's page context: a back chevron (back stack deeper than Home) and "idx/cnt" (paged groups). */
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
    lv_obj_t *b = make_label(bar, RA_GLYPH_CHEV_LEFT, &ra_st_txt_cap, &ra_icons_24, BACK_W);
    if (!b) return -1;
    lv_obj_set_pos(b, 4, 4);
    ui->back = b;
  }
  if (bc && bc->seq_cnt) {
    char sq[24];
    snprintf(sq, sizeof sq, "%u/%u", bc->seq_idx, bc->seq_cnt);
    int sw = lv_text_get_width(sq, (uint32_t)strlen(sq), &ra_font_r12, 0) + 2;
    lv_obj_t *q = make_label(bar, sq, &ra_st_txt_cap, NULL, sw);
    if (!q) return -1;
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(q, ra_color(TH->tx3), 0);
    lv_obj_set_pos(q, SEQ_RIGHT - sw, 9);
    ui->seq = q;
    tw = SEQ_RIGHT - sw - 6 - tx;
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
    if (ui->lbl[i]) lv_label_set_text(ui->lbl[i], tile_state_text(ui, &ui->page.widgets[i]));
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
  ui->msg = ui->cap = ui->msg_ico = ui->prog_fill = ui->bar = ui->title = ui->pct = ui->batt = ui->batt_fill = ui->back = ui->seq = NULL;
  ui->recv_total = 0;
  ui->rf[0] = ui->rf[1] = ui->rf[2] = NULL;
  np_forget(ui);
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
  ui->msg = ui->cap = ui->msg_ico = ui->prog_fill = NULL;
  ui->np_art = ui->np_t1 = ui->np_t2 = ui->np_gly = ui->np_tm = ui->np_bar = ui->np_fill = NULL;
  ui->recv_total = 0;
  ui->title_idx = -1;
  ui->pressed = -1;
  if (!enough_memory(BUILD_NEED(widgets, extra)) || bar_build(ui, scr, title, bc) < 0) {
    fail(ui);
    return -1;
  }
  return 0;
}

/* State screen: an optional 32 px glyph, a b20 title and an r12 caption, centred in the 288 px content area (with room
 * for the 6 px progress bar when `bar`). 0 ok, -1 out of memory. */
#define ST_ICON_H 32
#define ST_TITLE_H 24
#define ST_SUB_H 16
#define PROG_X 40
#define PROG_W 160
#define PROG_H 6

static int msg_sub_y; /* y of the caption of the state screen just built (the progress bar sits under it) */

static int message(ra_ui_t *ui, const char *glyph_s, const char *head, const char *sub, int head_bad, int bar) {
  lv_obj_t *scr = lv_screen_active();
  int h = (glyph_s ? ST_ICON_H + 6 : 0) + ST_TITLE_H + 6 + ST_SUB_H + (bar ? 12 + PROG_H : 0);
  int y = RA_BAR_H + (HP_PAGE_SCREEN_H - RA_BAR_H - h) / 2;
  ui->msg_bad = head_bad;
  if (glyph_s) {
    lv_obj_t *ic = make_label(scr, glyph_s, &ra_st_txt_cap, &ra_icons_32, 40);
    if (!ic) return -1;
    lv_obj_set_style_text_color(ic, ra_color(head_bad ? TH->bad : TH->acc), 0);
    lv_obj_set_pos(ic, (HP_PAGE_SCREEN_W - 40) / 2, y);
    ui->msg_ico = ic;
    y += ST_ICON_H + 6;
  }
  lv_obj_t *m = make_label(scr, head, &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
  if (!m) return -1;
  ui->msg = m;
  lv_obj_set_style_text_color(m, ra_color(head_bad ? TH->bad : TH->tx), 0);
  lv_obj_set_pos(m, RA_MARGIN, y);
  y += ST_TITLE_H + 6;
  lv_obj_t *c = make_label(scr, sub, &ra_st_txt_cap, NULL, 200);
  if (!c) return -1;
  ui->cap = c;
  lv_obj_set_pos(c, (HP_PAGE_SCREEN_W - 200) / 2, y);
  msg_sub_y = y;
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
  case HP_ROLE_PRIMARY: return WC(TH->s1, TH->acc, TH->acc, TH->tx2);
  case HP_ROLE_SECONDARY: return WC(TH->s1, TH->tx2, TH->tx3, TH->tx3);
  case HP_ROLE_DANGER: return WC(TH->s1, TH->bad, TH->bad, TH->bad);
  case HP_ROLE_NAV: return WC(TH->s1, TH->tx, TH->tx, TH->tx2);
  default: return WC(TH->s1, TH->tx, TH->tx, TH->tx2);
  }
}

static int widget_active(const ra_ui_t *ui, const hp_widget_t *w) {
  return ui->act_on && w->kind == HP_WIDGET_BUTTON && w->role != HP_ROLE_NAV && w->has_bind && w->bind == ui->act_idx;
}

/* The state a tile shows (r12, top right): "n/a" while unavailable, "stale" while the link is lost. */
static const char *tile_state_text(const ra_ui_t *ui, const hp_widget_t *w) {
  if (ui->link_lost && w->tstate != HP_TILE_ON) return "stale";
  return w->tstate == HP_TILE_UNAVAILABLE ? "n/a" : w->text;
}

/* Widget classes (docs/design remote redesign sizing table). A class fixes the icon size, the fonts and the layout, so a
 * widget looks the same whatever the page holds: ROW 24 px glyph + m16 label (+ 20 px chevron); CARD 32 px glyph over a b13
 * label; compact CARD 24 px glyph beside a b13 label of up to 2 lines; KEY 24 px glyph over an r12 caption; glyph KEY a
 * 24 px glyph alone; PILL a b20 digit or a 24 px glyph. Tiles are laid out by build_tile. */
typedef enum { WC_MONO, WC_ROW, WC_PILL, WC_CARD, WC_CCARD, WC_KEY } wclass_t;

static wclass_t wclass(const hp_widget_t *w, int bw, int bh) {
  if (w->shape == HP_SHAPE_MONOGRAM) return WC_MONO;
  if (w->shape == HP_SHAPE_ROW || (bh < 44 && bw >= 100)) return WC_ROW; /* wide and short: a row */
  if (w->shape == HP_SHAPE_PILL) return WC_PILL;
  if (bw >= 100) return w->compact || bh < 72 ? WC_CCARD : WC_CARD;
  return WC_KEY;
}

/* (Re)paints widget i from its role, state and the activity / link state: fill and ink colours only, so only that
 * widget's area is invalidated. */
static void np_paint(ra_ui_t *ui);
static void paint(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (!ui->obj[i]) return;
  if ((int)i == ui->np_idx) {
    np_paint(ui);
    return;
  }
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
  int tile = w->kind == HP_WIDGET_TILE;
  int key_cap = 0; /* a KEY's label is a caption */
  if (!tile && w->kind == HP_WIDGET_BUTTON && ui->ico[i] && ui->lbl[i]) {
    int x1, y1, x2, y2;
    ra_ui_widget_rect(&ui->page, w, &x1, &y1, &x2, &y2);
    key_cap = wclass(w, x2 - x1 + 1, y2 - y1 + 1) == WC_KEY;
  }
  if (ui->lbl[i]) lv_obj_set_style_text_color(ui->lbl[i], ra_color(tile || key_cap ? c.cap : c.ink), 0);
  if (ui->ico[i]) lv_obj_set_style_text_color(ui->ico[i], ra_color(c.icon), 0);
  if (tile && ui->wcap[i]) lv_obj_set_style_text_color(ui->wcap[i], ra_color(c.ink), 0);
  if (w->kind == HP_WIDGET_BUTTON) { /* app status: an accent ring (2 px, inside the box) and, in wcap, the play / pause glyph */
    int ring = w->tstate != HP_APP_NONE && !act;
    if (ring) {
      lv_obj_set_style_border_width(o, 2, 0);
      lv_obj_set_style_border_color(o, ra_color(TH->acc), 0);
    } else if (!c.local) {
      lv_obj_remove_local_style_prop(o, LV_STYLE_BORDER_WIDTH, 0);
      lv_obj_remove_local_style_prop(o, LV_STYLE_BORDER_COLOR, 0);
    }
    if (ui->wcap[i]) lv_obj_set_style_text_color(ui->wcap[i], ra_color(act ? TH->onacc : TH->acc), 0);
  }
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

/* The status glyph at the top right of an app button: play / pause while its status says so (16 px glyph, 12 px from the
 * edges, 8 on a compact card). 0 ok (also when skipped for memory: it is optional at run time), -1 creation failed. */
static int badge_sync(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (w->kind != HP_WIDGET_BUTTON || !ui->obj[i] || (int)i == ui->np_idx) return 0;
  if (w->tstate != HP_APP_PLAYING && w->tstate != HP_APP_PAUSED) {
    if (ui->wcap[i]) {
      lv_obj_delete(ui->wcap[i]);
      ui->wcap[i] = NULL;
    }
    return 0;
  }
  const char *g = w->tstate == HP_APP_PLAYING ? RA_GLYPH_PLAY : RA_GLYPH_PAUSE;
  if (ui->wcap[i]) {
    lv_label_set_text(ui->wcap[i], g);
    return 0;
  }
  if (!enough_memory(2048)) return 0;
  int x1, y1, x2, y2;
  ra_ui_widget_rect(&ui->page, w, &x1, &y1, &x2, &y2);
  int bw = x2 - x1 + 1, in = w->compact ? 8 : 12;
  lv_obj_t *b = make_label(ui->obj[i], g, &ra_st_txt_cap, &ra_icons_20, 24);
  if (!b) return -1;
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(b, bw - in - 22, in - 2);
  ui->wcap[i] = b;
  paint(ui, i);
  return 0;
}

#define PADX 4 /* the surface's left padding (theme.c): child positions are relative to the content box */
/* Child at absolute x (from the widget's left edge) and y. */
static void at(lv_obj_t *o, int x, int y) { lv_obj_set_pos(o, x - PADX, y); }

static int class_radius(wclass_t c, int bh) {
  switch (c) {
  case WC_PILL: return bh / 2;
  case WC_CARD:
  case WC_CCARD: return 14;
  default: return 10;
  }
}

/* A label of up to two lines (LVGL "..." after the second), as tall as its text. */
static lv_obj_t *make_label2(lv_obj_t *parent, const char *text, const lv_style_t *st, int w, int *h_out) {
  lv_obj_t *l = creation_fails() ? NULL : lv_label_create(parent);
  if (!l) return NULL;
  lv_obj_add_style(l, st, 0);
  const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
  int lh = lv_font_get_line_height(f);
  lv_point_t sz;
  lv_text_get_size(&sz, text, f, 0, 0, w, LV_TEXT_FLAG_NONE);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  *h_out = sz.y > lh ? 2 * lh : lh;
  lv_obj_set_size(l, w, *h_out);
  lv_label_set_text(l, text);
  return l;
}

/* A tile: glyph 20 top left, the state (r12) top right, the name (m16) at the bottom. lbl = state, wcap = name. */
static int build_tile(ra_ui_t *ui, unsigned i, lv_obj_t *o, int bw, int bh) {
  const hp_widget_t *w = &ui->page.widgets[i];
  char gb[4];
  const char *g = glyph(w->icon, gb);
  int roomy = bh >= 56 && bw >= 80;
  lv_obj_t *ic = NULL;
  if (g && roomy) {
    ic = make_label(o, g, &ra_st_txt_cap, &ra_icons_20, 24);
    if (!ic) return -1;
    at(ic, 8, 8);
  }
  int lh = lv_font_get_line_height(&ra_font_r12);
  int sx = ic ? 34 : 10; /* the state sits right of the glyph, right aligned */
  lv_obj_t *st = make_label(o, tile_state_text(ui, w), &ra_st_txt_cap, NULL, bw - sx - 10);
  if (!st) return -1;
  lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_RIGHT, 0);
  at(st, sx, roomy ? 12 : (bh - lh) / 2);
  ui->lbl[i] = st;
  ui->ico[i] = ic;
  if (w->label[0]) {
    int nh = lv_font_get_line_height(&ra_font_m16);
    int nw = roomy ? bw - 20 : bw - 20 - 44;
    lv_obj_t *nm = make_label(o, w->label, &ra_st_txt_body, NULL, nw);
    if (!nm) return -1;
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_LEFT, 0);
    at(nm, 10, roomy ? bh - 10 - nh : (bh - nh) / 2);
    ui->wcap[i] = nm;
  }
  return 0;
}

/* Builds the content of a v2 button / tile on its surface `o`. 0 ok, -1 out of memory. */
static int build_v2_content(ra_ui_t *ui, unsigned i, lv_obj_t *o, int bw, int bh) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (w->kind == HP_WIDGET_TILE) return build_tile(ui, i, o, bw, bh);
  char gb[4];
  const char *g = glyph(w->icon, gb);
  wclass_t c = wclass(w, bw, bh);
  int align = ui->page.fixed ? HP_ALIGN_LEFT : w->align; /* FIXED pages: rows and cards read left to right */
  lv_obj_t *l = NULL, *ic = NULL;
  const char *txt = w->label;
  char mono[8];
  int draw_label = !(w->icon_only && g) && w->label[0];
  if (c == WC_MONO) { /* a round badge: the icon, else the label's first letter */
    draw_label = !g && w->label[0];
    size_t k = first_char_len(w->label);
    memcpy(mono, w->label, k);
    mono[k] = 0;
    if (k == 1) mono[0] = (char)toupper((unsigned char)mono[0]);
    txt = mono;
    int mp = bw < bh ? bw : bh;
    int px = mp >= 72 ? 32 : mp >= 44 ? 24 : 20;
    if (g) {
      ic = make_label(o, g, &ra_st_txt_cap, icon_font(px), px + 4);
      if (!ic) return -1;
      lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
    } else if (draw_label) {
      l = make_label(o, txt, &ra_st_txt_display, NULL, bw - 8);
      if (!l) return -1;
      lv_obj_center(l);
    }
    ui->lbl[i] = l;
    ui->ico[i] = ic;
    return 0;
  }
  int px = c == WC_CARD ? 32 : 24;
  if ((c == WC_KEY && bh < 48) || (c == WC_ROW && bh < 36)) px = 20;
  if (g) {
    ic = make_label(o, g, &ra_st_txt_cap, icon_font(px), px + 4);
    if (!ic) return -1;
  }
  int ih = ic ? lv_font_get_line_height(icon_font(px)) : 0;
  if (ic && (!draw_label || c == WC_PILL)) { /* a glyph alone (glyph key, icon-only, no label, or a pill): centred */
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
    ui->ico[i] = ic;
    return 0;
  }
  if (!draw_label) return 0;
  switch (c) {
  case WC_ROW: {
    int chev = w->trailing ? 20 + 10 : 0, x0 = 10, gap = ic ? px + 10 : 0;
    int lw = bw - x0 - gap - 8 - chev;
    if (lw < 0) lw = 0;
    l = make_label(o, txt, &ra_st_txt_body, NULL, lw);
    if (!l) return -1;
    if (w->trailing) {
      lv_obj_t *ch = make_label(o, RA_GLYPH_CHEV_RIGHT, &ra_st_txt_cap, &ra_icons_20, 20);
      if (!ch) return -1;
      lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    if (align != HP_ALIGN_LEFT) { /* icon + label as one group, in the space left of the chevron */
      int room = bw - (w->trailing ? 8 + 20 + 10 : 0);
      int tw = lv_text_get_width(txt, (uint32_t)strlen(txt), lv_obj_get_style_text_font(l, 0), 0) + 2;
      if (tw < lw) {
        lv_obj_set_width(l, tw);
        lw = tw;
      }
      if (align == HP_ALIGN_RIGHT) {
        if (room - gap - lw - 10 >= 10) x0 = room - gap - lw - 10;
      } else if (room - gap - lw >= 20) x0 = (room - gap - lw) / 2;
    }
    if (ic) lv_obj_align(ic, LV_ALIGN_LEFT_MID, x0 - PADX, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x0 + gap - PADX, 0);
    break;
  }
  case WC_CARD: {
    int lh = lv_font_get_line_height(&ra_font_b13);
    int lw = bw - 24;
    int ix = align == HP_ALIGN_RIGHT ? bw - 10 - (px + 4) : align == HP_ALIGN_LEFT ? 10 : (bw - (px + 4)) / 2; /* the glyph cell is 4 px wider than the glyph */
    l = make_label(o, txt, ic ? &ra_st_txt_btn : &ra_st_txt_body, NULL, lw);
    if (!l) return -1;
    lv_obj_set_style_text_align(l, align == HP_ALIGN_LEFT ? LV_TEXT_ALIGN_LEFT : align == HP_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
    if (ic) {
      at(ic, ix, 12);
      at(l, 12, bh - 12 - lh);
    } else {
      at(l, 12, (bh - lv_font_get_line_height(&ra_font_m16)) / 2);
    }
    break;
  }
  case WC_CCARD: {
    int x0 = ic ? 10 + px + 10 : 10;
    int lh2 = 0;
    l = make_label2(o, txt, &ra_st_txt_btn, bw - x0 - 10, &lh2);
    if (!l) return -1;
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    if (ic) at(ic, 10 - 2, (bh - ih) / 2);
    at(l, x0, (bh - lh2) / 2);
    break;
  }
  case WC_PILL: {
    int digit = first_char_len(txt) + first_char_len(txt + first_char_len(txt)) == strlen(txt);
    l = make_label(o, txt, digit ? &ra_st_txt_display : &ra_st_txt_body, NULL, bw - 8);
    if (!l) return -1;
    lv_obj_center(l);
    break;
  }
  default: { /* KEY: glyph over an r12 caption (a label without a glyph: b13, centred) */
    if (!ic) {
      l = make_label(o, txt, &ra_st_txt_btn, NULL, bw - 6);
      if (!l) return -1;
      lv_obj_center(l);
      break;
    }
    int lh = lv_font_get_line_height(&ra_font_r12), gap = bh >= 52 ? 4 : 2;
    int top = (bh - ih - gap - lh) / 2;
    if (top < 0) top = 0;
    l = make_label(o, txt, &ra_st_txt_cap, NULL, bw - 6);
    if (!l) return -1;
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, top);
    at(l, 3, top + ih + gap);
    break;
  }
  }
  ui->lbl[i] = l;
  ui->ico[i] = ic;
  return 0;
}

static int np_build(ra_ui_t *ui, lv_obj_t *o, int bw, int bh);

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
    if (w->kind == HP_WIDGET_TILE) /* as build_tile: the glyph needs a 56 x 80 cell, the name is a label of its own */
      extra += (icon && bh >= 56 && bw >= 80) + (w->label[0] != 0);
    else if (w->kind == HP_WIDGET_BUTTON) extra += icon + (w->trailing != 0);
    extra += 2 * (w->color_fill.kind || w->color_on.kind); /* local styles */
    extra += w->kind == HP_WIDGET_BUTTON && (w->tstate == HP_APP_PLAYING || w->tstate == HP_APP_PAUSED);
  }
  if (v2 && ui->np_idx >= 0) extra += 9; /* the Now Playing card: art, 4 labels, bar */
  bar_ctx_t bc = {.back = v2 && ui->nav_depth > 1, .seq_idx = v2 ? p->seq_idx : 0, .seq_cnt = v2 ? p->seq_cnt : 0};
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
    if ((int)i == ui->np_idx) {
      if (np_build(ui, o, bw, bh) < 0) return fail(ui);
      continue;
    }
    if (v2) {
      int r = w->kind == HP_WIDGET_TILE ? 14 : w->shape == HP_SHAPE_MONOGRAM ? LV_RADIUS_CIRCLE : class_radius(wclass(w, bw, bh), bh);
      lv_obj_set_style_radius(o, r, 0);
      if (build_v2_content(ui, i, o, bw, bh) < 0) return fail(ui);
      paint(ui, i);
      if (badge_sync(ui, i) < 0) return fail(ui);
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
  if (!interactive_or_text && message(ui, NULL, "No Activities Yet", "Add some in Home Assistant", 0, 0) < 0) fail(ui);
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

static void np_prepare(ra_ui_t *ui);

void ra_ui_set_depth(ra_ui_t *ui, unsigned depth) { ui->nav_depth = depth > 255 ? 255 : (uint8_t)depth; }

void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p) {
  page_theme(ui, p);
  ui->page = *p;
  ui->fallback = 0;
  np_prepare(ui);
  build(ui);
}

static void show_builtin(ra_ui_t *ui, uint8_t id, const char *glyph_s, const char *head, const char *sub, int head_bad, int bar) {
  memset(&ui->page, 0, sizeof ui->page);
  np_forget(ui);
  ui->page.page_id = id;
  ui->page.cols = ui->page.rows = 1;
  if (screen_reset(ui, "Harmony", 0, 0, NULL) < 0) return;
  if (message(ui, glyph_s, head, sub, head_bad, bar) < 0) fail(ui);
}

/* Theme switched: recolour what carries a local colour (the shared styles were already updated in place). */
static void retheme(void *ctx) {
  ra_ui_t *ui = ctx;
  chrome_apply(ui);
  if (ui->seq) lv_obj_set_style_text_color(ui->seq, ra_color(TH->tx3), 0);
  if (ui->msg) lv_obj_set_style_text_color(ui->msg, ra_color(ui->msg_bad ? TH->bad : TH->tx), 0);
  if (ui->msg_ico) lv_obj_set_style_text_color(ui->msg_ico, ra_color(ui->msg_bad ? TH->bad : TH->acc), 0);
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
  ui->np_row = ui->np_idx = -1;
  ui->title_idx = ui->pressed = -1;
  /* The error state, built once while the pool is empty and kept hidden on the top layer. */
  ui->err = rect(lv_layer_top(), 0, 0, HP_PAGE_SCREEN_W, HP_PAGE_SCREEN_H, &ra_st_screen);
  if (ui->err) {
    lv_obj_t *m = make_label(ui->err, "Display Error", &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
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
  show_builtin(ui, RA_UI_PAGE_BOOT, RA_GLYPH_RF, "Connecting to Hub", "Looking for the hub", 0, 0);
  ui->fallback = 0;
}

void ra_ui_show_receiving(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_RECEIVING, RA_GLYPH_SYNC, "Receiving Pages", "Waiting for the first page", 0, 0);
  ui->fallback = 0;
}

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
  show_builtin(ui, RA_UI_PAGE_RECEIVING, RA_GLYPH_SYNC, "Receiving Pages", "", 0, 1);
  if (ui->page.page_id != RA_UI_PAGE_RECEIVING) return; /* the error state */
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *track = rect(scr, PROG_X, msg_sub_y + ST_SUB_H + 12, PROG_W, PROG_H, &ra_st_btn);
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
  show_builtin(ui, RA_UI_PAGE_FALLBACK, RA_GLYPH_RF_OFF, "Hub Unreachable", "Reconnecting automatically", 1, 0);
  ui->fallback = 1;
}

/* Live text / state of a tile on screen: the label text and colours only. */
static void tile_refresh(ra_ui_t *ui, unsigned i) {
  const hp_widget_t *w = &ui->page.widgets[i];
  if (ui->lbl[i]) lv_label_set_text(ui->lbl[i], ui->page.version == 2 ? tile_state_text(ui, w) : w->text);
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
  for (unsigned i = 0; i < ui->page.count; i++) { /* a button: its app status (ring, glyph) */
    hp_widget_t *w = &ui->page.widgets[i];
    if (w->id != tile_id || w->kind != HP_WIDGET_BUTTON || (int)i == ui->np_idx) continue;
    if (state > HP_APP_PAUSED) return -HP_E_INVAL;
    w->tstate = state;
    if (ui->obj[i]) {
      paint(ui, i);
      (void)badge_sync(ui, i);
    }
    return 0;
  }
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
  ui->np.flags = 0; /* the track belonged to the previous activity: the hub pushes a fresh now_playing */
  ui->np.cid = 0;
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

/* ---- Now Playing card ---- */

#define NP_PAD 4
#define NP_ART_W RA_COVER_W
#define NP_ART_H RA_COVER_H
#define NP_TX (NP_PAD + NP_ART_W + 10) /* text column x */

static lv_image_dsc_t cover_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = RA_COVER_W, .h = RA_COVER_H, .stride = RA_COVER_W * 2},
    .data_size = RA_COVER_W * RA_COVER_H * 2};

static void np_forget(ra_ui_t *ui) {
  ui->np_row = ui->np_idx = -1;
  ui->np_art = ui->np_t1 = ui->np_t2 = ui->np_gly = ui->np_tm = ui->np_bar = ui->np_fill = NULL;
  ui->np_cid_drawn = 0;
}

static int np_wanted(const ra_ui_t *ui) {
  const hp_page_t *p = &ui->page;
  return p->version == 2 && p->fixed && ui->np.flags && ui->act_on && p->act && p->act == ui->act_idx + 1;
}

/* Finds the grid row the card may replace (the first whose widgets are all 80 px buttons: span 12 or 6, not compact) and,
 * when the card is wanted, swaps the row's widgets for one span-12 button (the card). */
static void np_prepare(ra_ui_t *ui) {
  hp_page_t *p = &ui->page;
  np_forget(ui);
  if (p->version != 2 || !p->fixed) return;
  for (int y = 0; y < HP_PAGE_MAX_GRID && ui->np_row < 0; y++) {
    int any = 0, ok = 1;
    for (unsigned i = 0; i < p->count; i++) {
      const hp_widget_t *w = &p->widgets[i];
      if (w->y != y) continue;
      any = 1;
      if (w->kind != HP_WIDGET_BUTTON || w->shape != HP_SHAPE_CARD || w->w < 6 || w->compact) ok = 0;
    }
    if (any && ok) ui->np_row = y;
  }
  if (ui->np_row < 0 || !np_wanted(ui)) return;
  unsigned first = p->count, n = 0;
  for (unsigned i = 0; i < p->count; i++) {
    if (p->widgets[i].y != ui->np_row) {
      p->widgets[n++] = p->widgets[i];
      continue;
    }
    if (first == p->count) { /* the card takes the place of the row's first widget */
      first = n;
      memset(&p->widgets[n], 0, sizeof p->widgets[n]);
      p->widgets[n].kind = HP_WIDGET_BUTTON;
      p->widgets[n].shape = HP_SHAPE_CARD;
      p->widgets[n].y = (uint8_t)ui->np_row;
      p->widgets[n].w = 12;
      p->widgets[n].h = 1;
      n++;
    }
  }
  p->count = (uint8_t)n;
  ui->np_idx = (int)first;
}

int ra_ui_is_np(const ra_ui_t *ui, const hp_widget_t *w) { return ui->np_idx >= 0 && w == &ui->page.widgets[ui->np_idx]; }

static unsigned np_pos(const ra_ui_t *ui, uint64_t now) {
  unsigned p = ui->np.pos;
  if ((ui->np.flags & 1) && now > ui->np.at) p += (unsigned)((now - ui->np.at) / 1000);
  if (ui->np.dur && p > ui->np.dur) p = ui->np.dur;
  return p;
}

static void np_time_text(const ra_ui_t *ui, unsigned sec, char *b, size_t n) {
  const char *pre = ui->np.flags == 2 ? "Paused \xC2\xB7 " : "";
  if (ui->np.dur) snprintf(b, n, "%s%u:%02u / %u:%02u", pre, sec / 60, sec % 60, ui->np.dur / 60u, ui->np.dur % 60u);
  else snprintf(b, n, "%s%u:%02u", pre, sec / 60, sec % 60);
}

#define NP_TW (HP_PAGE_SCREEN_W - 2 * RA_MARGIN - NP_TX - 10) /* text column width */

static void np_draw_time(ra_ui_t *ui, unsigned sec) {
  char b[48];
  ui->np_sec_drawn = sec;
  np_time_text(ui, sec, b, sizeof b);
  if (ui->np_tm) lv_label_set_text(ui->np_tm, b);
  if (ui->np_fill) {
    int w = ui->np.dur ? (int)((unsigned long)NP_TW * (sec > ui->np.dur ? ui->np.dur : sec) / ui->np.dur) : 0;
    if (w > 0) lv_obj_set_width(ui->np_fill, w);
    set_hidden(ui->np_fill, w <= 0);
  }
}

/* The art slot: the cover when it is complete, else the Plex glyph. */
static int np_draw_art(ra_ui_t *ui) {
  if (!ui->np_art) return 0;
  lv_obj_clean(ui->np_art);
  ui->np_cid_drawn = 0;
  const uint16_t *px = ra_cover_expand(ui->np.cid);
  if (px) {
    lv_obj_t *im = creation_fails() ? NULL : lv_image_create(ui->np_art);
    if (!im) return -1;
    cover_dsc.data = (const uint8_t *)px;
    lv_image_set_src(im, &cover_dsc);
    lv_obj_set_pos(im, 0, 0);
    ui->np_cid_drawn = ui->np.cid;
    return 0;
  }
  lv_obj_t *g = make_label(ui->np_art, RA_GLYPH_PLEX, &ra_st_txt_cap, &ra_icons_32, NP_ART_W);
  if (!g) return -1;
  lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(g, ra_color(TH->tx2), 0);
  lv_obj_align(g, LV_ALIGN_CENTER, 0, 0);
  return 0;
}

static void np_paint(ra_ui_t *ui) {
  lv_obj_t *o = ui->np_idx >= 0 ? ui->obj[ui->np_idx] : NULL;
  if (!o) return;
  lv_obj_set_style_bg_color(o, ra_color(TH->s2), 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, ra_color(TH->acc), 0);
  if (ui->np_art) lv_obj_set_style_bg_color(ui->np_art, ra_color(TH->s3), 0);
  if (ui->np_t1) lv_obj_set_style_text_color(ui->np_t1, ra_color(TH->tx), 0);
  if (ui->np_t2) lv_obj_set_style_text_color(ui->np_t2, ra_color(TH->tx2), 0);
  if (ui->np_gly) lv_obj_set_style_text_color(ui->np_gly, ra_color(TH->acc), 0);
  if (ui->np_tm) lv_obj_set_style_text_color(ui->np_tm, ra_color(TH->acc), 0);
  if (ui->np_bar) lv_obj_set_style_bg_color(ui->np_bar, ra_color(TH->s3), 0);
  if (ui->np_fill) lv_obj_set_style_bg_color(ui->np_fill, ra_color(TH->acc), 0);
}

/* 224x80: 48x72 art at 4 px, then m16 title, r12 subtitle, an r12 accent line (glyph + time) and a 3 px bar. */
static int np_build(ra_ui_t *ui, lv_obj_t *o, int bw, int bh) {
  (void)bw;
  lv_obj_set_style_radius(o, RA_RADIUS_CARD, 0);
  ui->np_art = rect(o, NP_PAD, NP_PAD, NP_ART_W, NP_ART_H, &ra_st_ink);
  if (!ui->np_art) return -1;
  lv_obj_set_style_bg_opa(ui->np_art, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ui->np_art, 8, 0);
  lv_obj_set_style_clip_corner(ui->np_art, true, 0);
  int h1 = lv_font_get_line_height(&ra_font_m16), h2 = lv_font_get_line_height(&ra_font_r12);
  int y = (bh - (h1 + 2 + h2 + 4 + h2 + 4 + 3)) / 2;
  ui->np_t1 = make_label(o, ui->np.t1, &ra_st_txt_body, &ra_font_m16, NP_TW);
  if (!ui->np_t1) return -1;
  lv_obj_set_pos(ui->np_t1, NP_TX, y);
  y += h1 + 2;
  ui->np_t2 = make_label(o, ui->np.t2, &ra_st_txt_cap, &ra_font_r12, NP_TW);
  if (!ui->np_t2) return -1;
  lv_obj_set_pos(ui->np_t2, NP_TX, y);
  y += h2 + 4;
  ui->np_gly = make_label(o, ui->np.flags == 2 ? RA_GLYPH_PAUSE : RA_GLYPH_PLAY, &ra_st_txt_cap, &ra_icons_20, 20);
  ui->np_tm = make_label(o, "", &ra_st_txt_cap, &ra_font_r12, NP_TW - 18);
  if (!ui->np_gly || !ui->np_tm) return -1;
  lv_obj_set_style_text_align(ui->np_t1, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_text_align(ui->np_t2, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_text_align(ui->np_tm, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_pos(ui->np_gly, NP_TX - 2, y + (h2 - lv_font_get_line_height(&ra_icons_20)) / 2);
  lv_obj_set_pos(ui->np_tm, NP_TX + 18, y);
  y += h2 + 4;
  ui->np_bar = rect(o, NP_TX, y, NP_TW, 3, &ra_st_ink);
  if (!ui->np_bar) return -1;
  lv_obj_set_style_radius(ui->np_bar, 2, 0);
  ui->np_fill = rect(ui->np_bar, 0, 0, 1, 3, &ra_st_ink);
  if (!ui->np_fill) return -1;
  lv_obj_set_style_radius(ui->np_fill, 2, 0);
  if (np_draw_art(ui) < 0) return -1;
  np_draw_time(ui, np_pos(ui, ui->np.at));
  np_paint(ui);
  return 0;
}

int ra_ui_set_np(ra_ui_t *ui, const ra_np_t *np) {
  int shown = ui->np_idx >= 0;
  uint32_t old_cid = ui->np.cid;
  ui->np = *np;
  int want = np_wanted(ui) && ui->np_row >= 0;
  if (want != shown) return 1;
  if (!shown || ui->np_idx >= (int)ui->page.count || !ui->obj[ui->np_idx]) return 0;
  lv_label_set_text(ui->np_t1, np->t1);
  lv_label_set_text(ui->np_t2, np->t2);
  lv_label_set_text(ui->np_gly, np->flags == 2 ? RA_GLYPH_PAUSE : RA_GLYPH_PLAY);
  if (np->cid != old_cid || ui->np_cid_drawn != (ra_cover_have(np->cid) ? np->cid : 0)) {
    if (np_draw_art(ui) < 0) fail(ui);
  }
  np_draw_time(ui, np_pos(ui, np->at));
  return 0;
}

void ra_ui_np_cover(ra_ui_t *ui, uint32_t cid) {
  if (ui->np_idx < 0 || !ui->np_art || ui->np.cid != cid) return;
  if (np_draw_art(ui) < 0) fail(ui);
}

void ra_ui_np_tick(ra_ui_t *ui, uint64_t now) {
  if (ui->np_idx < 0 || !ui->np_tm || !(ui->np.flags & 1)) return;
  unsigned sec = np_pos(ui, now);
  if (sec != ui->np_sec_drawn) np_draw_time(ui, sec);
}

uint32_t ra_ui_np_next_ms(const ra_ui_t *ui, uint64_t now) {
  if (ui->np_idx < 0 || !(ui->np.flags & 1)) return UINT32_MAX;
  if (ui->np.dur && np_pos(ui, now) >= ui->np.dur) return UINT32_MAX; /* at the end: nothing moves */
  return 1000 - (uint32_t)((now - ui->np.at) % 1000);
}

void ra_ui_np_skip(ra_ui_t *ui, uint64_t now, uint64_t gap_ms) {
  if (!(ui->np.flags & 1)) return;
  unsigned long pos = np_pos(ui, now) + (unsigned long)(gap_ms / 1000); /* the monotonic clock stood still while asleep */
  if (ui->np.dur && pos > ui->np.dur) pos = ui->np.dur;
  ui->np.pos = pos > 0xFFFF ? 0xFFFF : (uint16_t)pos;
  ui->np.at = now;
}
