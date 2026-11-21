/* evdev -> logical key mapping, touch hit-testing, hold/repeat timing and wake-consume, against the
 * seam fake and an injected clock. */
#include "proto/gen/harmony_proto.h"
#include "remote_fake.h"

typedef struct {
  struct { uint8_t code, action; } e[256];
  int n;
  bool pass; /* what the wake gate answers */
  int gates, activities;
  hp_page_t page;
} rec_t;

static void r_emit(void *c, uint8_t code, uint8_t action) {
  rec_t *r = c;
  if (r->n < 256) { r->e[r->n].code = code; r->e[r->n++].action = action; }
}
static bool r_gate(void *c, uint64_t now) { (void)now; rec_t *r = c; r->gates++; return r->pass; }
static void r_act(void *c, uint64_t now) { (void)now; ((rec_t *)c)->activities++; }
static const hp_page_t *r_page(void *c) { return &((rec_t *)c)->page; }

static void setup_page(hp_page_t *p) {
  memset(p, 0, sizeof *p);
  p->cols = 4; p->rows = 6; p->count = 3;
  p->widgets[0] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 1, .x = 0, .y = 0, .w = 1, .h = 1};
  p->widgets[1] = (hp_widget_t){.kind = HP_WIDGET_TILE, .id = 2, .x = 1, .y = 0, .w = 2, .h = 1};
  p->widgets[2] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 3, .x = 0, .y = 1, .w = 2, .h = 1};
}

static fk_t fk;
static ra_sys_t sys;
static rec_t rec;
static ra_input_t in;

static void begin(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  ra_sys_init(&sys, &ops, NULL);
  memset(&rec, 0, sizeof rec);
  rec.pass = true;
  setup_page(&rec.page);
  ra_input_sink_t s = {.ctx = &rec, .emit = r_emit, .gate = r_gate, .activity = r_act, .page = r_page};
  CT_EQ(ra_input_open(&in, &sys, &s), 0);
}

static void feed(int type, int code, int value, uint64_t now) {
  struct input_event e = {.type = (uint16_t)type, .code = (uint16_t)code, .value = value};
  ra_input_feed(&in, &e, now);
}

static void test_all_35_keys(void) {
  begin();
  CT_EQ(HP_KEY_COUNT, 35);
  for (int i = 0; i < HP_KEY_COUNT; i++) {
    const hp_hwkey_t *k = &hp_hwkeys[i];
    rec.n = 0;
    feed(EV_KEY, k->evdev, 1, 1000);
    feed(EV_KEY, k->evdev, 0, 1100);
    CT_EQ(rec.n, 2);
    CT_EQ(rec.e[0].code, k->id);
    CT_EQ(rec.e[0].action, HP_KEY_ACTION_PRESS);
    CT_EQ(rec.e[1].code, k->id);
    CT_EQ(rec.e[1].action, HP_KEY_ACTION_RELEASE);
    CT_CHECK(k->id < HP_SOFT_KEY_BASE);
  }
  /* spot checks against the measured table, independent of the generated one */
  rec.n = 0;
  feed(EV_KEY, 115, 1, 0);
  feed(EV_KEY, 115, 0, 1);
  CT_EQ(rec.e[0].code, hp_hwkey_by_name("VolumeUp")->id);
  rec.n = 0;
  feed(EV_KEY, 102, 1, 0); /* Favorites, from the touch strip */
  feed(EV_KEY, 102, 0, 1);
  CT_EQ(rec.e[0].code, hp_hwkey_by_name("Favorites")->id);
  ra_input_close(&in);
}

static void test_unknown_dropped(void) {
  begin();
  int unknown[] = {0, 1, 30, 114 + 1000, 330 + 1, 500, 0xFFFF};
  for (unsigned i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
    feed(EV_KEY, unknown[i], 1, 1000);
    feed(EV_KEY, unknown[i], 0, 1050);
  }
  CT_EQ(rec.n, 0);
  CT_EQ(rec.gates, 0); /* an unknown key must not even wake the screen */
  CT_CHECK(!ra_input_any_held(&in));
  ra_input_tick(&in, 5000);
  CT_EQ(rec.n, 0);
  ra_input_close(&in);
}

static void tap(int x, int y, uint64_t t) {
  feed(EV_ABS, ABS_X, x, t);
  feed(EV_ABS, ABS_Y, y, t);
  feed(EV_KEY, BTN_TOUCH, 1, t);
  feed(EV_KEY, BTN_TOUCH, 0, t + 80);
}

static void test_touch(void) {
  begin();
  tap(10, 42, 1000); /* button id 1 (row 0 starts below the 32 px bar) */
  CT_EQ(rec.n, 2);
  CT_EQ(rec.e[0].code, HP_SOFT_KEY_BASE + 1);
  CT_EQ(rec.e[0].action, HP_KEY_ACTION_PRESS);
  CT_EQ(rec.e[1].code, HP_SOFT_KEY_BASE + 1);
  CT_EQ(rec.e[1].action, HP_KEY_ACTION_RELEASE);
  rec.n = 0;
  tap(100, 42, 2000); /* tile id 2 (cells x 1..2) */
  CT_EQ(rec.n, 2);
  CT_EQ(rec.e[0].code, HP_SOFT_KEY_BASE + 2);
  rec.n = 0;
  tap(10, 90, 3000); /* label: not tappable */
  tap(230, 300, 3100); /* empty cell */
  tap(10, 20, 3150); /* the status bar */
  tap(10, 320, 3200); /* ABS_Y max is 320: off-screen, no widget */
  CT_EQ(rec.n, 0);
  /* the release position decides: down on the button, slide off, release */
  feed(EV_ABS, ABS_X, 10, 4000);
  feed(EV_ABS, ABS_Y, 42, 4000);
  feed(EV_KEY, BTN_TOUCH, 1, 4000);
  feed(EV_ABS, ABS_X, 230, 4050);
  feed(EV_ABS, ABS_Y, 300, 4050);
  feed(EV_KEY, BTN_TOUCH, 0, 4100);
  CT_EQ(rec.n, 0);
  /* and the page under the finger is the one on screen now */
  rec.page.cols = 1; rec.page.rows = 1; rec.page.count = 1;
  rec.page.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 7, .x = 0, .y = 0, .w = 1, .h = 1};
  tap(239, 319, 5000);
  CT_EQ(rec.n, 2);
  CT_EQ(rec.e[0].code, HP_SOFT_KEY_BASE + 7);
  ra_input_close(&in);
}

static void test_hold_repeat(void) {
  begin();
  CT_EQ(RA_HOLD_DELAY_MS, 500);
  CT_EQ(RA_HOLD_REPEAT_MS, 200);
  feed(EV_KEY, 115, 1, 1000);
  CT_EQ(ra_input_next_ms(&in, 1000), 500);
  ra_input_tick(&in, 1499);
  CT_EQ(rec.n, 1); /* press only */
  ra_input_tick(&in, 1500);
  CT_EQ(rec.n, 2);
  CT_EQ(rec.e[1].action, HP_KEY_ACTION_HOLD);
  ra_input_tick(&in, 1650);
  CT_EQ(rec.n, 2);
  ra_input_tick(&in, 1700);
  ra_input_tick(&in, 1900);
  CT_EQ(rec.n, 4);
  feed(EV_KEY, 115, 2, 1950); /* kernel autorepeat is ignored */
  CT_EQ(rec.n, 4);
  feed(EV_KEY, 115, 0, 1950);
  CT_EQ(rec.n, 5);
  CT_EQ(rec.e[4].action, HP_KEY_ACTION_RELEASE);
  ra_input_tick(&in, 5000);
  CT_EQ(rec.n, 5); /* nothing after release */
  CT_EQ(ra_input_next_ms(&in, 5000), UINT32_MAX);
  /* a late tick yields one hold and re-arms ahead of now (no burst) */
  rec.n = 0;
  feed(EV_KEY, 114, 1, 10000);
  ra_input_tick(&in, 12600);
  CT_EQ(rec.n, 2);
  CT_CHECK(ra_input_next_ms(&in, 12600) <= RA_HOLD_REPEAT_MS && ra_input_next_ms(&in, 12600) > 0);
  /* two keys held at once track independently */
  feed(EV_KEY, 103, 1, 12700);
  feed(EV_KEY, 114, 0, 12800);
  CT_EQ(rec.e[rec.n - 1].code, hp_hwkey_by_evdev(114)->id);
  CT_CHECK(ra_input_any_held(&in));
  feed(EV_KEY, 103, 0, 12900);
  CT_CHECK(!ra_input_any_held(&in));
  ra_input_close(&in);
}

static void test_wake_consumes(void) {
  begin();
  rec.pass = false; /* the screen was off: this press only wakes it */
  feed(EV_KEY, 115, 1, 1000);
  ra_input_tick(&in, 3000);
  feed(EV_KEY, 115, 0, 3100);
  CT_EQ(rec.n, 0); /* neither press, hold nor release leaks */
  rec.pass = true;
  feed(EV_KEY, 115, 1, 4000);
  feed(EV_KEY, 115, 0, 4100);
  CT_EQ(rec.n, 2);
  /* touch: a consumed touch-down eats the whole tap */
  rec.n = 0;
  rec.pass = false;
  tap(10, 42, 5000);
  CT_EQ(rec.n, 0);
  rec.pass = true;
  tap(10, 42, 6000);
  CT_EQ(rec.n, 2);
  ra_input_close(&in);
}

/* The same mapping through the real read path: fds, select-style readiness, struct input_event. */
static void test_read_path(void) {
  begin();
  fk_key(&fk, 0, 115, 1);
  fk_key(&fk, 0, 115, 0);
  fk_key(&fk, 1, 364, 1); /* Home from the touch node */
  fk_key(&fk, 1, 364, 0);
  fk_touch(&fk, 10, 42, 1);
  fk_touch(&fk, 10, 42, 0);
  int ready[2];
  CT_EQ(fk.fds[in.fd[0]].dev, 0);
  CT_EQ(fk_wait(&fk, in.fd, 2, 0, ready), 2);
  ra_input_read(&in, 0, 1000);
  ra_input_read(&in, 1, 1000);
  CT_EQ(rec.n, 2 + 2 + 2);
  CT_EQ(rec.e[2].code, hp_hwkey_by_name("Home")->id);
  CT_EQ(rec.e[4].code, HP_SOFT_KEY_BASE + 1);
  CT_EQ(fk_wait(&fk, in.fd, 2, 0, ready), 0);
  ra_input_close(&in);
  CT_CHECK(fk_log_is_safe(&fk, NULL));
}

int main(void) {
  CT_RUN(test_all_35_keys);
  CT_RUN(test_unknown_dropped);
  CT_RUN(test_touch);
  CT_RUN(test_hold_repeat);
  CT_RUN(test_wake_consumes);
  CT_RUN(test_read_path);
  return CT_DONE();
}
