/* Screen refresh cost and touch latency of the remote UI, on the memory framebuffer and the injected clock:
 * flush calls / refresh cycles per page push, the --ui-debug counters, and touch -> pixels latency with an RF send
 * that blocks (the redraw must not wait for the radio). */
#include "remote_fake.h"
#include "remote/app/icons.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;

static uint16_t px(int x, int y) {
  const uint8_t *p = fk.fb + (size_t)y * 480 + (size_t)x * 2;
  return (uint16_t)(p[0] | (p[1] << 8));
}

static void fresh_app(void) {
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  CT_EQ(app_init(&app, &ops, &l, NULL), 0);
}

/* Step the loop until quiet (a few 40 ms ticks). */
static void settle(void) {
  for (int i = 0; i < 4; i++) {
    fk.now += 40;
    app_step(&app, 0);
  }
}

static hp_page_t worst_page(void) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = 40;
  p.cols = 8;
  p.rows = 4;
  for (unsigned i = 0; i < HP_PAGE_MAX_WIDGETS; i++) {
    hp_widget_t *w = &p.widgets[i];
    w->id = (uint8_t)(i + 1);
    w->x = (uint8_t)(i % 8);
    w->y = (uint8_t)(i / 8);
    w->w = w->h = 1;
    w->kind = (uint8_t)(i % 3 == 2 ? HP_WIDGET_TILE : HP_WIDGET_BUTTON);
    memset(w->label, 'W', HP_PAGE_MAX_LABEL);
    memset(w->text, 'M', HP_PAGE_MAX_TEXT);
  }
  p.count = HP_PAGE_MAX_WIDGETS;
  return p;
}

/* The gate page: a title and two buttons. */
static hp_page_t gate_page(void) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = 50;
  p.cols = 1;
  p.rows = 3;
  p.count = 3;
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .x = 0, .y = 0, .w = 1, .h = 1, .label = "Gate"};
  p.widgets[1] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 1, .x = 0, .y = 1, .w = 1, .h = 1, .label = "Open"};
  p.widgets[2] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 2, .x = 0, .y = 2, .w = 1, .h = 1, .label = "Close"};
  return p;
}

static void count_push(const char *name, hp_page_t p) {
  fresh_app();
  settle();
  app.display.ui_debug = getenv("UI_DEBUG") != NULL;
  unsigned f0 = app.display.flushes, c0 = app.display.cycles, px0 = app.display.flush_px;
  uint64_t r = 0;
  ra_ui_show_page(&app.ui, &p);
  settle();
  unsigned flushes = app.display.flushes - f0, cycles = app.display.cycles - c0;
  unsigned px = app.display.flush_px - px0;
  for (int i = 0; i < 6; i++) { /* mean render+flush time over 6 pushes (host CPU, indicative only) */
    ra_ui_show_page(&app.ui, &p);
    settle();
    r += app.display.last_render_ns + app.display.last_flush_ns;
  }
  fprintf(stderr, "%s: flushes=%u cycles=%u px=%u mean_render+flush_us=%u\n", name, flushes, cycles, px, (unsigned)(r / 6000));
  for (int i = 0; i < fk.nmsgs; i++)
    if (!strncmp(fk.msgs[i], "ui ", 3)) fprintf(stderr, "  %s\n", fk.msgs[i]);
  CT_EQ(cycles, 1); /* one page push is one refresh */
  CT_EQ(flushes, 1); /* ... flushed in one call */
  app_destroy(&app);
}

static void test_page_push_counts(void) {
  count_push("worst", worst_page());
  count_push("gate", gate_page());
}

static void test_counters(void) {
  fresh_app();
  settle();
  hp_page_t p = gate_page();
  ra_ui_show_page(&app.ui, &p);
  settle();
  /* Exactly the pixels flushed: a full-screen refresh is 240 x 320. */
  unsigned px0 = app.display.flush_px, f0 = app.display.flushes;
  lv_obj_invalidate(lv_screen_active());
  settle();
  CT_EQ(app.display.flush_px - px0, 240u * 320u);
  CT_EQ(app.display.flushes - f0, app.display.cyc_flushes);
  CT_EQ(app.display.cyc_px, 240u * 320u);
  app_destroy(&app);
}

static int in_pressed_color(int x, int y, uint16_t *out) {
  *out = px(x, y);
  return 1;
}

/* Touch -> flushed pixels within ~one frame, and the release redraw does not wait for a 50 ms RF send. */
static void test_touch_latency(void) {
  fresh_app();
  hp_page_t p = gate_page();
  ra_ui_show_page(&app.ui, &p);
  settle();
  int x1, y1, x2, y2;
  ra_ui_widget_rect(&p, &p.widgets[1], &x1, &y1, &x2, &y2);
  int cx = (x1 + x2) / 2, cy = y1 + 6; /* on the button, off its label */
  uint16_t idle, pressed;
  in_pressed_color(cx, cy, &idle);
  app.display.ui_debug = true;

  /* touch down */
  fk.now += 500;
  uint64_t t = fk.now;
  unsigned fr = app.display.frames;
  fk_touch(&fk, cx, cy, 1);
  app_step(&app, 0);
  CT_CHECK(app.display.frames > fr);
  in_pressed_color(cx, cy, &pressed);
  CT_CHECK(pressed != idle);
  CT_CHECK(app.display.last_flush_ms - t <= 20);
  CT_EQ(app.display.cyc_flushes, 1);
  CT_CHECK(app.display.cyc_px < 240u * 320u / 2); /* the press stays a partial redraw */
  fprintf(stderr, "press: touch_to_flush=%ums px=%u\n", (unsigned)(app.display.last_flush_ms - t), app.display.cyc_px);

  /* touch up with a blocking RF send */
  lk.block_ms = 50;
  lk.clk = &fk;
  fk.now += 500;
  t = fk.now;
  uint64_t before = fk.now;
  fr = app.display.frames;
  fk_touch(&fk, cx, cy, 0);
  app_step(&app, 0);
  CT_CHECK(app.display.frames > fr);
  uint16_t rel;
  in_pressed_color(cx, cy, &rel);
  CT_EQ(rel, idle);
  CT_CHECK(app.display.last_flush_ms - t <= 20); /* the pixels went out before the radio blocked */
  CT_EQ(lk.nsent, 2);
  CT_CHECK(fk.now - before >= 100); /* the fake did block (press + release) */
  fprintf(stderr, "release: touch_to_flush=%ums\n", (unsigned)(app.display.last_flush_ms - t));
  app_destroy(&app);
}

/* A page that arrives over the link is on the panel by the end of the same loop step (no wait for a 33 ms tick). */
static void test_page_push_is_immediate(void) {
  static const uint8_t PAGE1[] = {0x01, 0x01, 0x04, 0x06, 0x01, 0x00, 0x01, 0x00, 0x11, 0x04, 'V', 'o', 'l', '+'};
  fresh_app();
  settle();
  unsigned fr = app.display.frames, f0 = app.display.flushes;
  fkl_push_page(&lk, PAGE1, sizeof PAGE1);
  fk.now += 1; /* 1 ms after the last step: inside LVGL's refresh period */
  app_step(&app, 0);
  CT_EQ(app.ui.page.page_id, 1);
  CT_CHECK(app.display.frames > fr);
  CT_EQ(app.display.flushes - f0, 1);
  app_destroy(&app);
}

/* v2: a worst-case page (32 icons, captions) is one refresh and one flush; a press on an icon card is a partial redraw
 * within a frame; a nav tap and the Home key redraw BEFORE the radio write (which blocks 50 ms); Home is still forwarded. */
static hp_page_t v2_worst(void) {
  hp_page_t p = worst_page();
  p.version = 2;
  p.total = 3;
  p.gen = 1;
  p.kind = HP_PAGE_KIND_HOME;
  for (unsigned i = 0; i < HP_PAGE_MAX_WIDGETS; i++) {
    p.widgets[i].id = (uint8_t)i;
    p.widgets[i].icon = (uint8_t)(1 + i % RA_ICON_MAX);
    p.widgets[i].shape = (uint8_t)(i % 4);
    memset(p.widgets[i].label, 'W', 6);
    p.widgets[i].label[6] = 0;
  }
  return p;
}

static void test_v2_page_push_counts_and_press(void) {
  count_push("v2 worst", v2_worst());
  fresh_app();
  hp_page_t p = v2_worst();
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 0, .x = 0, .y = 0, .w = 1, .h = 1, .icon = RA_ICON_TV, .label = "TV"};
  ra_ui_show_page(&app.ui, &p);
  settle();
  fk.now += 500;
  uint64_t t = fk.now;
  unsigned fr = app.display.frames;
  fk_touch(&fk, 15, 40, 1); /* on widget 0 */
  app_step(&app, 0);
  CT_CHECK(app.display.frames > fr);
  CT_CHECK(app.display.last_flush_ms - t <= 20);
  CT_EQ(app.display.cyc_flushes, 1);
  CT_CHECK(app.display.cyc_px < 240u * 320u / 4); /* one small widget */
  app_destroy(&app);
}

static void test_v2_nav_and_home_redraw_before_the_radio(void) {
  fresh_app();
  /* Home (kind 1): one nav widget (button + style + icon + bind) to page 1; page 1: empty, titled "Other" */
  uint8_t home[] = {2, 0, 1, 2, 1, 7, 2, 1, 0, 0, 0, 0x70, 1, 0x00, 0x11, 4 /* style: role nav */, RA_ICON_CHEV_RIGHT, 1, 1, 'N'};
  uint8_t other[] = {2, 1, 1, 2, 0, 7, 2, 0, 0, 0, 5, 'O', 't', 'h', 'e', 'r'};
  fkl_push_page(&lk, home, sizeof home);
  fkl_push_page(&lk, other, sizeof other);
  settle();
  CT_EQ(app.ui.page.page_id, 0);
  lk.block_ms = 50;
  lk.clk = &fk;
  fk.now += 500;
  uint64_t t = fk.now, before = fk.now;
  unsigned fr = app.display.frames;
  int sent = lk.nsent;
  fk_touch(&fk, 120, 120, 1);
  app_step(&app, 0);
  fk.now += 40;
  fk_touch(&fk, 120, 120, 0);
  app_step(&app, 0);
  CT_EQ(app.ui.page.page_id, 1); /* the nav tap switched the page */
  CT_EQ(lk.nsent, sent);         /* and sent nothing: the fake clock did not block either */
  CT_CHECK(fk.now - before < 100);
  CT_CHECK(app.display.frames > fr);
  (void)t;
  /* Home key: the page goes back at once, the radio write (50 ms) comes after the pixels, and the key is forwarded */
  fk.now += 500;
  t = fk.now;
  fr = app.display.frames;
  fk_key(&fk, 1, 102, 1);
  app_step(&app, 0);
  CT_EQ(app.ui.page.page_id, 0);
  CT_CHECK(app.display.frames > fr);
  CT_CHECK(app.display.last_flush_ms - t <= 20); /* flushed before the blocking send */
  CT_EQ(lk.nsent, sent + 1);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_v2_page_push_counts_and_press);
  CT_RUN(test_v2_nav_and_home_redraw_before_the_radio);
  CT_RUN(test_page_push_is_immediate);
  CT_RUN(test_page_push_counts);
  CT_RUN(test_counters);
  CT_RUN(test_touch_latency);
  return CT_DONE();
}
