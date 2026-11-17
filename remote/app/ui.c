#include "remote/app/ui.h"

#include <string.h>

#include "hub/hp_errors.h"

#define BTN_BG 0x2A6FDB
#define TILE_BG 0x2B3138
#define TILE_BORDER 0x4A5560
#define TEXT_FG 0xF0F0F0
#define INSET 2 /* visual gap between neighbouring buttons/tiles */

static int edge(int i, int n, int total) { return (i * total + n - 1) / n; }

void ra_ui_widget_box(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2) {
  *x1 = edge(w->x, p->cols, HP_PAGE_SCREEN_W);
  *x2 = edge(w->x + w->w, p->cols, HP_PAGE_SCREEN_W) - 1;
  *y1 = edge(w->y, p->rows, HP_PAGE_SCREEN_H);
  *y2 = edge(w->y + w->h, p->rows, HP_PAGE_SCREEN_H) - 1;
}

static void plain(lv_obj_t *o) {
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_set_style_outline_width(o, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, int w) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TEXT_FG), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, text);
  return l;
}

static void build(ra_ui_t *ui) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(RA_UI_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  memset(ui->obj, 0, sizeof ui->obj);
  memset(ui->lbl, 0, sizeof ui->lbl);
  const hp_page_t *p = &ui->page;
  for (unsigned i = 0; i < p->count; i++) {
    const hp_widget_t *w = &p->widgets[i];
    int x1, y1, x2, y2;
    ra_ui_widget_box(p, w, &x1, &y1, &x2, &y2);
    int bw = x2 - x1 + 1, bh = y2 - y1 + 1;
    lv_obj_t *o;
    if (w->kind == HP_WIDGET_LABEL) {
      /* Transparent box that clips the text to the widget's cells. */
      o = lv_obj_create(scr);
      plain(o);
      lv_obj_set_pos(o, x1, y1);
      lv_obj_set_size(o, bw, bh);
      lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(o, 0, 0);
      lv_obj_set_style_pad_all(o, 0, 0);
      lv_obj_t *l = make_label(o, w->label, &lv_font_montserrat_14, bw);
      lv_obj_center(l);
      ui->obj[i] = o;
      ui->lbl[i] = l;
      continue;
    }
    int in = bw > 2 * INSET + 2 && bh > 2 * INSET + 2 ? INSET : 0;
    o = w->kind == HP_WIDGET_BUTTON ? lv_button_create(scr) : lv_obj_create(scr);
    plain(o);
    lv_obj_set_pos(o, x1 + in, y1 + in);
    lv_obj_set_size(o, bw - 2 * in, bh - 2 * in);
    lv_obj_set_style_pad_all(o, 2, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (w->kind == HP_WIDGET_BUTTON) {
      lv_obj_set_style_bg_color(o, lv_color_hex(BTN_BG), 0);
      lv_obj_set_style_border_width(o, 0, 0);
    } else {
      lv_obj_set_style_bg_color(o, lv_color_hex(TILE_BG), 0);
      lv_obj_set_style_border_color(o, lv_color_hex(TILE_BORDER), 0);
      lv_obj_set_style_border_width(o, 1, 0);
    }
    const char *txt = w->kind == HP_WIDGET_BUTTON ? w->label : w->text;
    lv_obj_t *l = make_label(o, txt, w->kind == HP_WIDGET_TILE ? &lv_font_montserrat_20 : &lv_font_montserrat_14,
                             bw - 2 * in - 6);
    lv_obj_center(l);
    ui->obj[i] = o;
    ui->lbl[i] = l;
  }
}

void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p) {
  ui->page = *p;
  ui->fallback = 0;
  build(ui);
}

/* A built-in page: one centred label across the whole screen. */
static void show_builtin(ra_ui_t *ui, uint8_t id, const char *text) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = id;
  p.cols = p.rows = 1;
  p.count = 1;
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .x = 0, .y = 0, .w = 1, .h = 1};
  strncpy(p.widgets[0].label, text, HP_PAGE_MAX_LABEL);
  ui->page = p;
  build(ui);
}

void ra_ui_init(ra_ui_t *ui) {
  memset(ui, 0, sizeof *ui);
  ra_ui_show_boot(ui);
}

void ra_ui_show_boot(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_BOOT, "Harmony");
  ui->fallback = 0;
}

void ra_ui_show_fallback(ra_ui_t *ui) {
  show_builtin(ui, RA_UI_PAGE_FALLBACK, "Hub unreachable");
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
