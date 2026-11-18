#include "remote/app/ui.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hub/hp_errors.h"

#define ELLIPSIS "\xe2\x80\xa6"

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
  lv_color_t rf = RA_COLOR(ui->link_lost ? RA_HEX_BAD : RA_HEX_TX2);
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(ui->rf[i], rf, 0);
    if (i) set_hidden(ui->rf[i], ui->link_lost); /* lost: one short red bar */
  }
  int low = ui->soc >= 0 && ui->soc <= RA_LOWBATT_PCT && !ui->charging;
  lv_color_t c = RA_COLOR(low ? RA_HEX_BAD : ui->charging ? RA_HEX_ACC : RA_HEX_TX2);
  lv_obj_set_style_border_color(ui->batt, c, 0);
  lv_obj_set_style_bg_color(ui->batt_fill, c, 0);
  lv_obj_set_style_text_color(ui->pct, RA_COLOR(low ? RA_HEX_BAD : RA_HEX_TX2), 0);
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

/* 0 ok, -1 out of memory (the caller shows the error state; chrome_apply ignores a partial bar). */
static int bar_build(ra_ui_t *ui, lv_obj_t *scr, const char *title) {
  ui->bar = ui->title = ui->pct = ui->batt = ui->batt_fill = NULL;
  ui->rf[0] = ui->rf[1] = ui->rf[2] = NULL;
  lv_obj_t *bar = rect(scr, 0, 0, HP_PAGE_SCREEN_W, RA_BAR_H, &ra_st_bar);
  if (!bar) return -1;
  lv_obj_t *t = make_label(bar, title, &ra_st_txt_title, NULL, TITLE_W);
  if (!t) return -1;
  lv_obj_set_pos(t, TITLE_X, 6);
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
  ui->msg = ui->cap = ui->bar = ui->title = ui->pct = ui->batt = ui->batt_fill = NULL;
  ui->rf[0] = ui->rf[1] = ui->rf[2] = NULL;
  ui->page.count = 0;
  ui->page.page_id = RA_UI_PAGE_ERROR;
  ui->title_idx = ui->pressed = -1;
  ui->fallback = 0;
  ui->oom++;
  if (ui->err) lv_obj_remove_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
}

/* Free memory needed before building `widgets` widgets: ~0.9 KiB each measured, plus the bar and the renderer's
 * working set. Refusing early is what keeps LVGL's own allocations (which do not check for failure) safe. */
#define BUILD_NEED(widgets) (1100u * (widgets) + 8192u)

static int enough_memory(unsigned need) {
  if (ra_ui_test_skip_precheck) return 1;
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  return mon.free_size >= need;
}

static int screen_reset(ra_ui_t *ui, const char *title, unsigned widgets) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_remove_style_all(scr);
  lv_obj_add_style(scr, &ra_st_screen, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  if (ui->err) lv_obj_add_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
  memset(ui->obj, 0, sizeof ui->obj);
  memset(ui->lbl, 0, sizeof ui->lbl);
  ui->msg = ui->cap = NULL;
  ui->title_idx = -1;
  ui->pressed = -1;
  if (!enough_memory(BUILD_NEED(widgets)) || bar_build(ui, scr, title) < 0) {
    fail(ui);
    return -1;
  }
  return 0;
}

/* Centred two-line state message in the content area; 0 ok, -1 out of memory. */
static int message(ra_ui_t *ui, const char *head, const char *sub, uint32_t head_hex) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *m = make_label(scr, head, &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
  if (!m) return -1;
  ui->msg = m;
  lv_obj_set_style_text_color(m, RA_COLOR(head_hex), 0);
  lv_obj_set_pos(m, RA_MARGIN, 148);
  lv_obj_t *c = make_label(scr, sub, &ra_st_txt_cap, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
  if (!c) return -1;
  ui->cap = c;
  lv_obj_set_pos(c, RA_MARGIN, 176);
  return 0;
}

static void build(ra_ui_t *ui) {
  lv_obj_t *scr = lv_screen_active();
  const hp_page_t *p = &ui->page;
  const char *title = "Harmony";
  int title_idx = hp_page_title(p);
  if (title_idx >= 0 && p->widgets[title_idx].label[0]) title = p->widgets[title_idx].label;
  if (screen_reset(ui, title, p->count) < 0) return;
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
      continue;
    }
    int btn = w->kind == HP_WIDGET_BUTTON;
    o = rect(scr, x1, y1, bw, bh, btn ? &ra_st_btn : &ra_st_card);
    if (!o) return fail(ui);
    lv_obj_add_style(o, btn ? &ra_st_btn_pr : &ra_st_card_pr, LV_STATE_PRESSED);
    const char *txt = btn ? w->label : w->text;
    const lv_style_t *ts = btn ? (bw >= 100 ? &ra_st_txt_body : &ra_st_txt_btn) : (bw >= 80 ? &ra_st_txt_display : &ra_st_txt_btn);
    l = make_label(o, txt, ts, NULL, bw - 8);
    if (!l) return fail(ui);
    lv_obj_center(l);
    if (btn && !strcasecmp(w->label, "Power Off")) lv_obj_set_style_text_color(l, RA_COLOR(RA_HEX_BAD), 0);
    ui->obj[i] = o;
    ui->lbl[i] = l;
  }
  if (!interactive_or_text && message(ui, "No activities yet", "Add some in Home Assistant", RA_HEX_TX) < 0) fail(ui);
}

void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p) {
  ui->page = *p;
  ui->fallback = 0;
  build(ui);
}

static void show_builtin(ra_ui_t *ui, uint8_t id, const char *head, const char *sub, uint32_t head_hex) {
  memset(&ui->page, 0, sizeof ui->page);
  ui->page.page_id = id;
  ui->page.cols = ui->page.rows = 1;
  if (screen_reset(ui, "Harmony", 0) < 0) return;
  if (message(ui, head, sub, head_hex) < 0) fail(ui);
}

void ra_ui_init(ra_ui_t *ui) {
  memset(ui, 0, sizeof *ui);
  ui->soc = -1;
  ui->title_idx = ui->pressed = -1;
  /* The error state, built once while the pool is empty and kept hidden on the top layer. */
  ui->err = rect(lv_layer_top(), 0, 0, HP_PAGE_SCREEN_W, HP_PAGE_SCREEN_H, &ra_st_screen);
  if (ui->err) {
    lv_obj_t *m = make_label(ui->err, "Display error", &ra_st_txt_display, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
    lv_obj_t *c = make_label(ui->err, "Waiting for the next page", &ra_st_txt_cap, NULL, HP_PAGE_SCREEN_W - 2 * RA_MARGIN);
    if (m) {
      lv_obj_set_style_text_color(m, RA_COLOR(RA_HEX_BAD), 0);
      lv_obj_set_pos(m, RA_MARGIN, 148);
    }
    if (c) lv_obj_set_pos(c, RA_MARGIN, 176);
    lv_obj_add_flag(ui->err, LV_OBJ_FLAG_HIDDEN);
  }
  ra_ui_show_boot(ui);
}

void ra_ui_show_boot(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_BOOT, "Connecting" ELLIPSIS, "Looking for the hub", RA_HEX_TX);
  ui->fallback = 0;
}

void ra_ui_show_receiving(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_RECEIVING, "Receiving config" ELLIPSIS, "Waiting for the first page", RA_HEX_TX);
  ui->fallback = 0;
}

void ra_ui_show_fallback(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_FALLBACK, "Hub unreachable", "Reconnecting automatically", RA_HEX_BAD);
  ui->fallback = 1;
}

int ra_ui_tile_text(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, const char *text, size_t len) {
  if (page_id != ui->page.page_id) return -HP_E_NOENT;
  int r = hp_page_set_tile_text(&ui->page, tile_id, text, len);
  if (r < 0) return r;
  for (unsigned i = 0; i < ui->page.count; i++)
    if (ui->page.widgets[i].id == tile_id && ui->page.widgets[i].kind == HP_WIDGET_TILE && ui->lbl[i])
      lv_label_set_text(ui->lbl[i], ui->page.widgets[i].text);
  return 0;
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
