#ifndef HP_REMOTE_UI_H
#define HP_REMOTE_UI_H

/* Page -> LVGL widgets. Cell edges use the same integer mapping as hp_page_hit(), so a tapped pixel
 * and the widget drawn under it always agree: cell column c spans px [ceil(c*240/cols), ceil((c+1)*240/cols)). */
#include "lvgl.h"
#include "remote/page/page.h"

#define RA_UI_PAGE_BOOT 0xFE     /* built-in pages use ids a real page cannot be mistaken for... */
#define RA_UI_PAGE_FALLBACK 0xFF /* ...but tile_update on them finds no tile and is ignored */

#define RA_UI_BG 0x101418

typedef struct {
  hp_page_t page; /* what is on screen (boot / fallback / pushed) */
  lv_obj_t *obj[HP_PAGE_MAX_WIDGETS];
  lv_obj_t *lbl[HP_PAGE_MAX_WIDGETS];
  int fallback; /* 1 while the "Hub unreachable" page is shown */
} ra_ui_t;

void ra_ui_init(ra_ui_t *ui); /* needs an LVGL display; shows the boot page */
void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p);
void ra_ui_show_boot(ra_ui_t *ui);
void ra_ui_show_fallback(ra_ui_t *ui);
/* 0 ok; <0 as hp_page_set_tile_text, or -HP_E_NOENT when page_id is not the page on screen. */
int ra_ui_tile_text(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, const char *text, size_t len);
/* Pixel box of grid cell span, same maths as the hit-tester. */
void ra_ui_widget_box(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2);

#endif
