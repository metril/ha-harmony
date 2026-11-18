/* LVGL pool behaviour of the remote UI: peak usage and leaks over many page/state/toast cycles, and survival
 * of out-of-memory (precheck and injected creation failures). Separate from test_remote_render so ctest runs
 * the two in parallel. */
#include "remote_fake.h"

static fk_t fk;
static app_t app;

static __attribute__((unused)) uint16_t px(int x, int y) {
  const uint8_t *p = fk.fb + (size_t)y * 480 + (size_t)x * 2;
  return (uint16_t)(p[0] | (p[1] << 8));
}
#define RGB565(hex) ((uint16_t)(((((hex) >> 16) & 0xF8) << 8) | ((((hex) >> 8) & 0xFC) << 3) | (((hex) & 0xFF) >> 3)))

static void refresh(void) { lv_refr_now(app.display.disp); }

static void fresh_app(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), 0);
}

static __attribute__((unused)) hp_page_t home_page(void) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = 20;
  p.cols = 1;
  p.rows = 4;
  p.count = 3;
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .x = 0, .y = 0, .w = 1, .h = 1, .label = "Activities"};
  p.widgets[1] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 1, .x = 0, .y = 1, .w = 1, .h = 1, .label = "Watch TV"};
  p.widgets[2] = (hp_widget_t){.kind = HP_WIDGET_TILE, .id = 2, .x = 0, .y = 2, .w = 1, .h = 1, .text = "21.5 C"};
  return p;
}
static __attribute__((unused)) void sc_home(void) {
  hp_page_t p = home_page();
  ra_ui_show_page(&app.ui, &p);
}
static __attribute__((unused)) void sc_boot(void) { ra_ui_show_boot(&app.ui); }
static __attribute__((unused)) void sc_receiving(void) { ra_ui_show_receiving(&app.ui); }
static __attribute__((unused)) void sc_fallback(void) { ra_ui_show_fallback(&app.ui); }

/* ---- memory: peak, leaks, and out-of-memory survival ---- */

/* Worst case for the pool: 32 widgets with maximum-length labels / tile text (optionally one is the title). */
static hp_page_t worst_page(int id, int titled) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = (uint8_t)id;
  p.cols = 8;
  p.rows = titled ? 5 : 4;
  unsigned n = 0;
  if (titled) p.widgets[n++] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .x = 0, .y = 0, .w = 8, .h = 1, .label = "Maximum length title"};
  for (unsigned i = 0; n < HP_PAGE_MAX_WIDGETS; i++, n++) {
    hp_widget_t *w = &p.widgets[n];
    w->id = (uint8_t)(i + 1);
    w->x = (uint8_t)(i % 8);
    w->y = (uint8_t)((titled ? 1 : 0) + i / 8);
    w->w = w->h = 1;
    w->kind = (uint8_t)(i % 3 == 2 ? HP_WIDGET_TILE : HP_WIDGET_BUTTON);
    memset(w->label, 'W', HP_PAGE_MAX_LABEL);
    memset(w->text, 'M', HP_PAGE_MAX_TEXT);
  }
  p.count = (uint8_t)n;
  return p;
}

/* Worst case for a version 2 page: 32 widgets, every one with an icon and a maximum-length label; tiles with a caption
 * and the longest value; paged + BACK (the bar's extra labels). Mixed shapes. */
static hp_page_t worst_page_v2(int id, int stale_tiles) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.version = 2;
  p.page_id = (uint8_t)id;
  p.total = 50;
  p.gen = 3;
  p.cols = 8;
  p.rows = 4;
  p.back = 1;
  p.seq_idx = 2;
  p.seq_cnt = 3;
  memset(p.title, 'T', HP_PAGE_MAX_TITLE);
  for (unsigned i = 0; i < HP_PAGE_MAX_WIDGETS; i++) {
    hp_widget_t *w = &p.widgets[i];
    w->id = (uint8_t)i;
    w->x = (uint8_t)(i % 8);
    w->y = (uint8_t)(i / 8);
    w->w = w->h = 1;
    w->kind = (uint8_t)(i % 3 == 2 ? HP_WIDGET_TILE : HP_WIDGET_BUTTON);
    w->icon = (uint8_t)(1 + i % RA_ICON_MAX);
    w->role = (uint8_t)(i % 5);
    w->shape = (uint8_t)(i % 4);
    w->has_bind = i % 2;
    w->bind = (uint8_t)(i % 7);
    memset(w->label, 'W', HP_PAGE_MAX_LABEL);
    memset(w->text, 'M', HP_PAGE_MAX_TEXT);
    w->tstate = (uint8_t)(i % 4);
  }
  p.count = HP_PAGE_MAX_WIDGETS;
  (void)stale_tiles;
  return p;
}

static size_t pool_free(void) {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  return m.free_size;
}

static void test_pool_peak_and_no_leak(void) {
  fresh_app();
  hp_page_t w1 = worst_page(40, 0), w2 = worst_page(41, 1);
  size_t baseline = 0;
  for (int i = 0; i < 20; i++) {
    ra_ui_show_page(&app.ui, i % 2 ? &w2 : &w1);
    ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger");
    ra_ui_set_battery(&app.ui, 8, 0);
    ra_ui_set_link(&app.ui, i % 2);
    ra_ui_press_at(&app.ui, 120, 100);
    refresh();
    ra_ui_show_fallback(&app.ui); /* a state-screen transition with the toast still up */
    ra_ui_show_receiving(&app.ui);
    ra_ui_toast_dismiss(&app.ui);
    ra_ui_show_boot(&app.ui);
    CT_EQ(app.ui.oom, 0);
    if (i == 0) baseline = pool_free();
    else CT_EQ(pool_free(), baseline); /* nothing leaks from cycle to cycle */
  }
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  unsigned pct = (unsigned)(m.max_used * 100 / LV_MEM_SIZE);
  fprintf(stderr, "pool peak: %zu of %u bytes (%u%%)\n", m.max_used, (unsigned)LV_MEM_SIZE, pct);
  CT_CHECK(pct <= 60);
  app_destroy(&app);
}

/* 32 widgets with icons (an extra label each), captions on the tiles, a toast over it, the stale state and activity
 * changes: peak <= 60 % of the pool, nothing leaks cycle to cycle, no out-of-memory state. */
static void test_v2_pool_peak_and_no_leak(void) {
  fresh_app();
  hp_page_t w1 = worst_page_v2(10, 0), w2 = worst_page_v2(11, 1);
  size_t baseline = 0;
  for (int i = 0; i < 8; i++) {
    ra_ui_show_page(&app.ui, i % 2 ? &w2 : &w1);
    ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger");
    ra_ui_set_battery(&app.ui, 8, 0);
    ra_ui_set_link(&app.ui, 1); /* "stale" captions */
    ra_ui_set_activity(&app.ui, (uint8_t)(i % 7), 1);
    ra_ui_press_at(&app.ui, 120, 100);
    ra_ui_tile_state(&app.ui, (uint8_t)(10 + i % 2), 2, HP_TILE_ON, "On", 2);
    refresh();
    ra_ui_set_link(&app.ui, 0);
    ra_ui_show_receiving_n(&app.ui, 5, 40);
    ra_ui_show_receiving_n(&app.ui, 6, 40);
    ra_ui_toast_dismiss(&app.ui);
    ra_ui_show_boot(&app.ui);
    CT_EQ(app.ui.oom, 0);
    if (i == 0) baseline = pool_free();
    else CT_EQ(pool_free(), baseline);
  }
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  unsigned pct = (unsigned)(m.max_used * 100 / LV_MEM_SIZE);
  fprintf(stderr, "v2 pool peak: %zu of %u bytes (%u%%)\n", m.max_used, (unsigned)LV_MEM_SIZE, pct);
  CT_CHECK(pct <= 60);
  app_destroy(&app);
}

/* argv[1]: "v1" or "v2" runs one half (ctest runs them in parallel); no argument runs both. */
int main(int argc, char **argv) {
  int v1 = argc < 2 || !strcmp(argv[1], "v1"), v2 = argc < 2 || !strcmp(argv[1], "v2");
  if (v1) CT_RUN(test_pool_peak_and_no_leak);
  if (v2) CT_RUN(test_v2_pool_peak_and_no_leak);
  return CT_DONE();
}
