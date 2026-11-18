/* Robustness of the remote app's v2 event path: random sequences of valid, mutated and out-of-order pages (mixed gens,
 * totals, versions, v1 pages), tile updates for missing pages / tiles, state updates, link events, keys and touches, under
 * ASan/UBSan. Invariants after every step: the app never shows a stored page that is not in the current set; and after a
 * Home press the app is on a page (the start page in v2 mode). argv[1] = shard (0..3). */
#include "remote_fake.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;
static uint64_t rng_s;
static uint32_t rnd(uint32_t n) {
  if (!n) return 0;
  rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
  return (uint32_t)((rng_s >> 11) % n);
}

/* a valid-ish v2 page: 4x4 grid, up to 5 widgets, random roles / icons / binds; may be rejected by the decoder */
static size_t gen_page(uint8_t *o, unsigned id, unsigned total, unsigned gen) {
  size_t k = 0;
  unsigned n = rnd(6), cnt = rnd(3) ? 0 : 2 + rnd(2), idx = cnt ? 1 + rnd(cnt) : 0;
  o[k++] = 2; o[k++] = (uint8_t)id; o[k++] = 4; o[k++] = 4; o[k++] = (uint8_t)n;
  o[k++] = (uint8_t)gen; o[k++] = (uint8_t)total; o[k++] = (uint8_t)(rnd(3) | (rnd(2) ? 4 : 0));
  o[k++] = (uint8_t)((idx << 4) | cnt); o[k++] = (uint8_t)(rnd(3) ? 0 : 1 + rnd(3));
  unsigned tl = rnd(5);
  o[k++] = (uint8_t)tl;
  for (unsigned i = 0; i < tl; i++) o[k++] = (uint8_t)('a' + rnd(26));
  for (unsigned i = 0; i < n; i++) {
    unsigned kind = rnd(3), role = kind == 1 ? 0 : rnd(5), icon = rnd(2) ? 0 : 1 + rnd(RA_ICON_MAX);
    unsigned style = role | (rnd(4) << 3);
    int bind = role == HP_ROLE_NAV ? (int)rnd(total) : rnd(3) ? -1 : (int)rnd(4);
    o[k++] = (uint8_t)(kind | (style ? 0x10 : 0) | (icon ? 0x20 : 0) | (bind >= 0 ? 0x40 : 0));
    o[k++] = (uint8_t)i;
    o[k++] = (uint8_t)(((i % 4) << 4) | (i / 4));
    o[k++] = 0x11;
    if (style) o[k++] = (uint8_t)style;
    if (icon) o[k++] = (uint8_t)icon;
    if (bind >= 0) o[k++] = (uint8_t)bind;
    unsigned ll = 1 + rnd(4);
    o[k++] = (uint8_t)ll;
    for (unsigned j = 0; j < ll; j++) o[k++] = (uint8_t)('A' + rnd(26));
  }
  return k;
}

static void ev_page(const uint8_t *d, size_t n) {
  app_link_event_t *e = &lk.q[lk.nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_PAGE_PUSH;
  memcpy(e->data, d, n);
  e->len = n;
}

static void gen_event(void) {
  static const uint8_t V1[] = {0x01, 0x01, 0x04, 0x06, 0x01, 0x00, 0x01, 0x00, 0x11, 0x04, 'V', 'o', 'l', '+'};
  uint8_t raw[HP_PAGE_MAX_BYTES];
  static unsigned st = 4, sg = 1; /* the current config: changes now and then */
  if (rnd(30) == 0) { st = 1 + rnd(6); sg = 1 + rnd(2); }
  unsigned total = rnd(10) ? st : 1 + rnd(6), gen = rnd(10) ? sg : 1 + rnd(2);
  switch (rnd(12)) {
  case 0: case 1: case 2: case 3: { /* a valid page (the total is sometimes 0 or over 32: refused) */
    unsigned t = rnd(12) == 0 ? (rnd(2) ? 0 : 33 + rnd(200)) : total;
    ev_page(raw, gen_page(raw, rnd(t ? t : 1), t, gen));
    break;
  }
  case 4: { /* a mutated one: flipped bytes, truncated or extended */
    size_t n = gen_page(raw, rnd(total), total, gen);
    for (unsigned i = rnd(4); i--;) raw[rnd((uint32_t)n)] ^= (uint8_t)(1u << rnd(8));
    if (rnd(3) == 0) n = rnd((uint32_t)n + 1);
    else if (rnd(4) == 0 && n + 8 < sizeof raw) { for (unsigned i = 0; i < 8; i++) raw[n++] = (uint8_t)rnd(256); }
    ev_page(raw, n);
    break;
  }
  case 5: if (rnd(4) == 0) ev_page(V1, sizeof V1); break;
  case 6: case 7: { /* tile text / state for any page and tile, present or not */
    app_link_event_t *e = &lk.q[lk.nq++];
    memset(e, 0, sizeof *e);
    e->kind = rnd(2) ? APP_LINK_EV_TILE_STATE : APP_LINK_EV_TILE_UPDATE;
    e->page = (uint8_t)(rnd(5) ? rnd(8) : rnd(256));
    e->tile = (uint8_t)(rnd(5) ? rnd(6) : rnd(256));
    e->state = (uint8_t)rnd(5);
    unsigned l = rnd(5) ? rnd(5) : 40 + rnd(10);
    for (unsigned i = 0; i < l && i < HP_PAGE_MAX_TEXT; i++) e->text[i] = (char)(rnd(10) == 0 ? 0xC3 : 'a' + rnd(26)); /* some invalid UTF-8 */
    break;
  }
  case 8: { /* state_update */
    app_link_event_t *e = &lk.q[lk.nq++];
    memset(e, 0, sizeof *e);
    e->kind = APP_LINK_EV_STATE_UPDATE;
    e->activity = (uint8_t)rnd(5);
    e->power = rnd(2) != 0;
    break;
  }
  case 9: {
    app_link_event_t *e = &lk.q[lk.nq++];
    memset(e, 0, sizeof *e);
    e->kind = rnd(3) ? APP_LINK_EV_ALIVE : APP_LINK_EV_FAILED;
    e->fatal = rnd(8) == 0;
    break;
  }
  case 10: { /* a key */
    static const int K[][2] = {{0, 103}, {0, 108}, {0, 174}, {1, 102}, {1, 364}};
    int i = (int)rnd(5);
    fk.now += 30; fk_key(&fk, K[i][0], K[i][1], 1); app_step(&app, 0);
    fk.now += (rnd(8) == 0) ? 700 : 30; fk_key(&fk, K[i][0], K[i][1], 0);
    break;
  }
  default: { /* a touch anywhere */
    int x = (int)rnd(240), y = (int)rnd(320);
    fk.now += 30; fk_touch(&fk, x, y, 1); app_step(&app, 0);
    fk.now += 30; fk_touch(&fk, x, y, 0);
    break;
  }
  }
}

static void check_invariants(void) {
  const hp_page_t *p = &app.ui.page;
  if (p->version == 2 && p->page_id < 0xFC) {
    CT_CHECK(app.v2);
    CT_CHECK(hp_store_has(&app.store, p->page_id));
    CT_CHECK(p->page_id < hp_store_need(&app.store));
    CT_CHECK(app.cur == p->page_id);
  }
  CT_CHECK(hp_store_count(&app.store) <= HP_STORE_MAX);
}

static void run_seed(uint64_t seed, int loops) {
  rng_s = seed * 0x9E3779B97F4A7C15ull + 1;
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  CT_EQ(app_init(&app, &ops, &l, NULL), 0);
  for (int i = 0; i < loops; i++) {
    if (lk.qh == lk.nq) lk.qh = lk.nq = 0;
    for (unsigned n = 1 + rnd(3); n-- && lk.nq < 56;) gen_event();
    fk.now += rnd(40) == 0 ? 11000 : 40; /* now and then past the receiving timeout */
    app_step(&app, 0);
    app_step(&app, 0);
    check_invariants();
    if (rnd(60) == 0) ra_ui_test_fail_at = (int)rnd(20); /* an out-of-memory build somewhere */
    else if (rnd(3) == 0) ra_ui_test_fail_at = -1;
  }
  ra_ui_test_fail_at = -1;
  /* wherever the chaos left it, Home gets to a page (twice: the first press may only wake the screen) */
  for (int k = 0; k < 2; k++) {
    fk.now += 100; fk_key(&fk, 1, 102, 1); app_step(&app, 0);
    fk.now += 50; fk_key(&fk, 1, 102, 0); app_step(&app, 0);
  }
  for (int k = 0; k < 3; k++) { fk.now += 40; app_step(&app, 0); }
  check_invariants();
  if (getenv("FUZZ_STATS")) fprintf(stderr, "seed %llu v2=%d count=%u total=%u oom=%u page=%d\n", (unsigned long long)seed, app.v2, hp_store_count(&app.store), hp_store_need(&app.store), app.ui.oom, app.ui.page.page_id);
  if (app.v2 && hp_store_count(&app.store)) {
    CT_EQ(app.ui.page.version, 2);
    CT_EQ(app.ui.page.page_id, hp_store_start(&app.store));
  } else if (!app.v2) {
    CT_CHECK(app.ui.page.version == 1 || app.ui.page.page_id >= 0xFC);
  }
  app_destroy(&app);
}

int main(int argc, char **argv) {
  int shard = argc > 1 ? atoi(argv[1]) : -1;
  for (int s = 0; s < 8; s++)
    if (shard < 0 || s % 4 == shard) run_seed((uint64_t)s + 1, 250);
  return CT_DONE();
}
