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

static size_t pool_free(void) {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  return m.free_size;
}

static int err_shown(void) { return !lv_obj_has_flag(app.ui.err, LV_OBJ_FLAG_HIDDEN); }

static int red_in_error_text(void) {
  int red = 0;
  for (int y = 148; y < 172; y++)
    for (int x = 8; x < 232; x++) red += px(x, y) == RGB565(RA_HEX_BAD);
  return red;
}

static void test_out_of_memory_survives(void) {
  /* (a) the pool is nearly full: the free-memory precheck refuses the build and shows the error state */
  fresh_app();
  hp_page_t w = worst_page(40, 0);
  static void *hog[1024];
  for (size_t keep = 56000; keep > 0; keep -= 3500) {
    ra_ui_show_boot(&app.ui);
    int n = 0;
    while (pool_free() > keep && n < 1024) hog[n++] = lv_malloc(128);
    unsigned oom0 = app.ui.oom;
    ra_ui_show_page(&app.ui, &w);
    int refused = app.ui.oom != oom0;
    for (int i = 0; i < n; i++) lv_free(hog[i]);
    refresh();
    if (refused) {
      CT_CHECK(err_shown());
      CT_EQ(app.ui.page.page_id, RA_UI_PAGE_ERROR);
      CT_EQ(app.ui.page.count, 0);
      CT_CHECK(red_in_error_text() > 50);
    }
    /* the next page recovers: error state hidden, page drawn */
    ra_ui_show_page(&app.ui, &w);
    refresh();
    CT_CHECK(!err_shown());
    CT_EQ(app.ui.oom, oom0 + refused);
    CT_EQ(app.ui.page.page_id, 40);
  }
  CT_CHECK(app.ui.oom > 0); /* the squeeze did trip it */
  app_destroy(&app);

  /* (b) an object creation fails at every possible point of every kind of screen: no crash, error state shown */
  void (*screens[])(void) = {sc_home, sc_boot, sc_receiving, sc_fallback};
  for (int s = 0; s < 5; s++) {
    for (int n = 0; n < 200; n++) {
      if (s == 4 && n % 6) continue; /* the 33-object page: every 6th creation point is plenty */
      fresh_app();
      hp_page_t big = worst_page(41, 1);
      unsigned oom0 = app.ui.oom;
      ra_ui_test_fail_at = n;
      if (s == 4) ra_ui_show_page(&app.ui, &big);
      else screens[s]();
      int triggered = ra_ui_test_fail_at < 0; /* the hook counted down to the Nth creation */
      ra_ui_test_fail_at = -1;
      refresh();
      if (triggered) {
        CT_CHECK(app.ui.oom > oom0);
        CT_CHECK(err_shown());
        CT_EQ(app.ui.page.count, 0);
        CT_CHECK(red_in_error_text() > 50);
        sc_home(); /* and it recovers */
        refresh();
        CT_CHECK(!err_shown());
        CT_EQ(app.ui.page.page_id, 20);
        app_destroy(&app);
      } else {
        CT_EQ(app.ui.oom, oom0); /* n is past the last creation: the screen built fine */
        app_destroy(&app);
        break;
      }
    }
  }
  /* a failing toast is simply skipped */
  fresh_app();
  ra_ui_test_fail_at = 1;
  ra_ui_toast(&app.ui, "x", "y");
  ra_ui_test_fail_at = -1;
  CT_CHECK(app.ui.toast == NULL && app.ui.toast_timer == NULL);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_out_of_memory_survives);
  return CT_DONE();
}
