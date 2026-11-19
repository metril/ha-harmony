/* theme_push on the remote app: the one custom-theme slot next to the page store, in every arrival order. The rule: a page of
 * theme id 7 counts as received (remote_info gen/pages, display) only while the slot holds the theme of its gen; a bad theme
 * changes nothing; a new gen clears the store; same gen + other bytes repaints; same gen + same bytes is a no-op. */
#include "cJSON.h"
#include "remote/app/theme.h"
#include "remote/app/theme_derive.h"
#include "remote_fake.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;

static void start(void) {
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  CT_EQ(app_init(&app, &ops, &l, NULL), 0);
}
static void step(void) {
  fk.now += 40;
  app_step(&app, 0);
}
static void settle(void) {
  for (int i = 0; i < 4; i++) step();
}
static uint64_t fb_hash(void) {
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  lv_refr_now(app.display.disp);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < 480 * 320; i++) h = (h ^ fk.fb[i]) * 0x100000001b3ull;
  return h;
}

/* a v2 page: id of total, gen, theme id, one button */
static void page(uint8_t id, uint8_t total, uint8_t gen, uint8_t theme) {
  const uint8_t b[] = {2, id, 2, 2, 1, gen, total, (uint8_t)(theme << 3), 0, 0, 0, 0, 1, 0x00, 0x11, 1, 'X'};
  fkl_push_page(&lk, b, sizeof b);
}
static void theme_ev(uint8_t gen, const uint8_t *w, size_t n) {
  app_link_event_t *e = &lk.q[lk.nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_THEME_PUSH;
  e->gen = gen;
  memcpy(e->data, w, n);
  e->len = n;
}
/* a wire theme: daylight with accent `acc` */
static void wire(uint8_t *w, uint32_t acc) {
  ra_theme_t t = ra_themes[RA_THEME_DAYLIGHT];
  t.acc = acc;
  const uint32_t tok[13] = {t.bg, t.s1, t.s2, t.s3, t.tx, t.tx2, t.tx3, t.acc, t.onacc, t.warm, t.onwarm, t.bad, t.onbad};
  size_t n = 0;
  w[n++] = RA_WIRE_VERSION;
  for (int i = 0; i < 13; i++) {
    uint16_t v = ra_to565(tok[i]);
    w[n++] = (uint8_t)(v >> 8);
    w[n++] = (uint8_t)v;
  }
  for (int i = 0; i < RA_PALETTE_N; i++) {
    const uint32_t c[3] = {t.palette[i].fill, t.palette[i].ink, t.palette[i].pressed};
    for (int k = 0; k < 3; k++) {
      uint16_t v = ra_to565(c[k]);
      w[n++] = (uint8_t)(v >> 8);
      w[n++] = (uint8_t)v;
    }
  }
  CT_EQ(n, (size_t)RA_WIRE_LEN);
}
static bool shows(uint8_t id) { return app.ui.page.version == 2 && app.ui.page.page_id == id && !app.ui.recv_total; }

static void test_theme_then_pages(void) {
  start();
  uint8_t w[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  theme_ev(5, w, sizeof w);
  settle();
  CT_EQ(hp_store_count(&app.store), 0u); /* a theme alone is not a set */
  page(0, 2, 5, 7);
  settle();
  CT_CHECK(!hp_store_complete(&app.store));
  page(1, 2, 5, 7);
  settle();
  CT_CHECK(hp_store_complete(&app.store));
  CT_CHECK(shows(0));
  CT_EQ(ra_theme_cur()->id, RA_THEME_CUSTOM_ID);
  CT_EQ(ra_theme_cur()->acc, ra_from565(ra_to565(0x8048C0)));
  app_destroy(&app);
}

/* pages first: they never render, never count; the late theme completes the set and shows it */
static void test_pages_then_late_theme(void) {
  start();
  page(0, 2, 5, 7);
  page(1, 2, 5, 7);
  settle();
  CT_EQ(hp_store_count(&app.store), 2u);
  CT_CHECK(!hp_store_complete(&app.store));
  CT_CHECK(hp_store_theme_missing(&app.store));
  CT_CHECK(!shows(0));
  fk.now += 20000; /* past the partial timeout: still no page with a missing theme */
  settle();
  CT_CHECK(!shows(0) && !shows(1));
  uint8_t w[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  theme_ev(5, w, sizeof w);
  settle();
  CT_CHECK(hp_store_complete(&app.store));
  CT_CHECK(shows(0) || shows(1));
  CT_EQ(ra_theme_cur()->id, RA_THEME_CUSTOM_ID);
  app_destroy(&app);
}

/* a set of presets only needs no theme at all */
static void test_preset_set_needs_no_theme(void) {
  start();
  page(0, 2, 5, 1);
  page(1, 2, 5, 2);
  settle();
  CT_CHECK(hp_store_complete(&app.store));
  CT_CHECK(shows(0));
  CT_CHECK(!hp_store_theme_missing(&app.store));
  app_destroy(&app);
}

/* a theme of another gen clears the held set (like a page of a new gen) and the old pages are never redrawn with it */
static void test_theme_of_other_gen_clears_the_set(void) {
  start();
  uint8_t w[RA_WIRE_LEN], w2[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  wire(w2, 0x30A060);
  theme_ev(5, w, sizeof w);
  page(0, 1, 5, 7);
  settle();
  CT_CHECK(shows(0));
  theme_ev(6, w2, sizeof w2);
  settle();
  CT_EQ(hp_store_count(&app.store), 0u);
  CT_CHECK(!shows(0)); /* "Receiving", not the old page in the new colours */
  page(0, 1, 6, 7);
  settle();
  CT_CHECK(shows(0));
  CT_EQ(ra_theme_cur()->acc, ra_from565(ra_to565(0x30A060)));
  /* pages of gen 7 against the slot of gen 6: held, not complete, not shown */
  page(0, 1, 7, 7);
  settle();
  CT_CHECK(!hp_store_complete(&app.store) && !shows(0));
  app_destroy(&app);
}

static void test_duplicate_is_idempotent_and_collision_replaces(void) {
  start();
  uint8_t w[RA_WIRE_LEN], w2[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  wire(w2, 0x30A060);
  theme_ev(5, w, sizeof w);
  page(0, 1, 5, 7);
  settle();
  uint64_t h1 = fb_hash();
  theme_ev(5, w, sizeof w); /* the hub resent it */
  settle();
  CT_CHECK(shows(0) && hp_store_complete(&app.store));
  CT_CHECK(fb_hash() == h1);
  theme_ev(5, w2, sizeof w2); /* same gen, other bytes: replaced and repainted, the set stays complete */
  settle();
  CT_CHECK(shows(0) && hp_store_complete(&app.store));
  CT_EQ(hp_store_count(&app.store), 1u);
  CT_EQ(ra_theme_cur()->acc, ra_from565(ra_to565(0x30A060)));
  CT_CHECK(fb_hash() != h1);
  app_destroy(&app);
}

static void test_corrupt_theme_changes_nothing(void) {
  start();
  uint8_t w[RA_WIRE_LEN], bad[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  theme_ev(5, w, sizeof w);
  page(0, 1, 5, 7);
  settle();
  uint64_t h1 = fb_hash();
  memcpy(bad, w, sizeof w);
  bad[0] = 0; /* version 0 */
  theme_ev(5, bad, sizeof bad);
  theme_ev(6, bad, sizeof bad); /* and of a new gen: must not clear the set */
  theme_ev(5, w, sizeof w - 1);  /* short */
  settle();
  CT_CHECK(shows(0) && hp_store_complete(&app.store));
  CT_EQ(app.store.theme_gen, 5);
  CT_CHECK(!memcmp(app.store.theme, w, sizeof w));
  CT_CHECK(fb_hash() == h1);
  app_destroy(&app);
}

/* a restart has neither the slot nor the set: pages of theme 7 wait for the theme again */
static void test_restart_has_no_slot(void) {
  start();
  uint8_t w[RA_WIRE_LEN];
  wire(w, 0x8048C0);
  theme_ev(5, w, sizeof w);
  page(0, 1, 5, 7);
  settle();
  CT_CHECK(hp_store_complete(&app.store));
  app_destroy(&app);
  start();
  CT_EQ(hp_store_count(&app.store), 0u);
  CT_CHECK(!app.store.have_theme);
  page(0, 1, 5, 7);
  settle();
  CT_CHECK(!hp_store_complete(&app.store) && !shows(0));
  CT_EQ(ra_theme_cur()->id, RA_THEME_DEFAULT);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_theme_then_pages);
  CT_RUN(test_pages_then_late_theme);
  CT_RUN(test_preset_set_needs_no_theme);
  CT_RUN(test_theme_of_other_gen_clears_the_set);
  CT_RUN(test_duplicate_is_idempotent_and_collision_replaces);
  CT_RUN(test_corrupt_theme_changes_nothing);
  CT_RUN(test_restart_has_no_slot);
  return CT_DONE();
}
