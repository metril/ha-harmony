/* The remote app's RF link end to end on the host: the real app (memory framebuffer, scripted
 * input) -> rf_link -> rf_spi (remote role) -> fake /dev/rfspi <-> radio bridge <-> fake hub
 * /dev/rfspi -> rf_spi (hub role) -> hub-side hp_rf_link_t + the real payload codec. Plus the
 * battery heartbeat, CLI parsing, clean shutdown and the brick-safety replay over BOTH syscall
 * seams (the app's and rf_spi's). Time is a fake clock; the rf_spi reader threads are real. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>

#include "cJSON.h"
#include "hub/hp_errors.h"
#include "hub/io/rf_spi.h"
#include "hub/rf/link.h"
#include "hub/rf/rf_payload.h"
#include "remote/app/cli.h"
#include "remote/app/icons.h"
#include "remote/app/rf_link.h"
#include "remote/app/theme.h"
#include "remote_fake.h"

#ifndef HP_PAGES_JSON
#error "HP_PAGES_JSON and HP_RENDER_JSON must be defined"
#endif

/* ---- fake /dev/rfspi (one per chip) with a radio bridge between the two ---- */

typedef struct fake {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int is_remote;
  int hal; /* hal_alive result */
  char opens[8][64];
  int open_flags[8], nopens, nclosed;
  struct { size_t len; uint8_t b0, b2; uint8_t head[7]; } w[512];
  int nw;
  size_t max_write;
  uint8_t rx[64][64];
  size_t rxlen[64];
  int rxh, rxn, intr;
  _Atomic uint64_t now;
  struct fake *peer;
  int drop_acks; /* swallow this many outgoing ack frames */
  int drop_all;  /* swallow every outgoing data frame */
  int fail_sync; /* the 10-byte sync pulse write fails with EIO */
} fake_t;

static void feed(fake_t *f, const uint8_t *b, size_t n) {
  pthread_mutex_lock(&f->mu);
  if (f->rxn < 64) {
    memcpy(f->rx[(f->rxh + f->rxn) % 64], b, n);
    f->rxlen[(f->rxh + f->rxn) % 64] = n;
    f->rxn++;
  }
  pthread_cond_broadcast(&f->cv);
  pthread_mutex_unlock(&f->mu);
}

static int f_open(void *c, const char *p, int fl) {
  fake_t *f = c;
  pthread_mutex_lock(&f->mu);
  if (f->nopens < 8) {
    snprintf(f->opens[f->nopens], 64, "%s", p);
    f->open_flags[f->nopens] = fl;
  }
  f->nopens++;
  pthread_mutex_unlock(&f->mu);
  return 42;
}

static ssize_t f_write(void *c, int fd, const void *b, size_t n) {
  fake_t *f = c;
  (void)fd;
  const uint8_t *p = b;
  if (n == 10 && f->fail_sync) {
    errno = EIO;
    return -1;
  }
  pthread_mutex_lock(&f->mu);
  if (f->nw < 512) {
    f->w[f->nw].len = n;
    f->w[f->nw].b0 = p[0];
    f->w[f->nw].b2 = n > 2 ? p[2] : 0;
    memcpy(f->w[f->nw].head, p, n < 7 ? n : 7);
  }
  f->nw++;
  if (n > f->max_write) f->max_write = n;
  int drop = 0;
  if (n >= 30) {
    const uint8_t *inner = p + (f->is_remote ? 4 : 2);
    if (f->drop_all) drop = 1;
    else if ((inner[0] & 0xE0) == 0x40 && f->drop_acks > 0) {
      f->drop_acks--;
      drop = 1;
    }
  }
  fake_t *peer = f->peer;
  pthread_mutex_unlock(&f->mu);
  if (peer && !drop && n >= 30) {
    uint8_t o[32];
    if (f->is_remote && n == 32 && p[0] == 0x20) { /* the remote is slot 2 on the hub (bench) */
      o[0] = 0x12, o[1] = 2;
      memcpy(o + 2, p + 4, 28);
      feed(peer, o, 30);
    } else if (!f->is_remote && n == 30 && p[0] == 0x12) { /* the hub is slot 1 on the remote (bench) */
      o[0] = 0x20, o[1] = 1, o[2] = 0x12, o[3] = 1;
      memcpy(o + 4, p + 2, 28);
      feed(peer, o, 32);
    }
  }
  return 0;
}

static ssize_t f_read(void *c, int fd, void *b, size_t cap) {
  fake_t *f = c;
  (void)fd; (void)cap;
  pthread_mutex_lock(&f->mu);
  while (!f->rxn && !f->intr) pthread_cond_wait(&f->cv, &f->mu);
  ssize_t r;
  if (f->rxn) {
    r = (ssize_t)f->rxlen[f->rxh];
    memcpy(b, f->rx[f->rxh], f->rxlen[f->rxh]);
    f->rxh = (f->rxh + 1) % 64;
    f->rxn--;
  } else {
    f->intr = 0;
    errno = EINTR;
    r = -1;
  }
  pthread_mutex_unlock(&f->mu);
  return r;
}

static int f_close(void *c, int fd) {
  fake_t *f = c;
  (void)fd;
  pthread_mutex_lock(&f->mu);
  f->nclosed++;
  pthread_mutex_unlock(&f->mu);
  return 0;
}
static int f_hal(void *c) { return ((fake_t *)c)->hal; }
static uint64_t f_now(void *c) { return ((fake_t *)c)->now; }
static void f_interrupt(void *c, pthread_t t) {
  fake_t *f = c;
  (void)t;
  pthread_mutex_lock(&f->mu);
  f->intr = 1;
  pthread_cond_broadcast(&f->cv);
  pthread_mutex_unlock(&f->mu);
}

static void fake_init(fake_t *f, int is_remote) {
  memset(f, 0, sizeof *f);
  pthread_mutex_init(&f->mu, NULL);
  pthread_cond_init(&f->cv, NULL);
  f->is_remote = is_remote;
  f->now = 1000;
}
static hp_rf_spi_ops_t fake_ops(fake_t *f) {
  return (hp_rf_spi_ops_t){f, f_open, f_write, f_read, f_close, f_hal, f_now, f_interrupt};
}

/* ---- the world ---- */

typedef struct { uint8_t remote, key, action; } hkey_t;
typedef struct { uint8_t remote, pct; uint16_t mv; uint64_t at; int ord; } hbatt_t;
typedef struct { uint8_t remote, fmt, caps, icons, gen, pages; uint64_t at; int ord; } hinfo_t;

static fk_t fk;
static app_t app;
static rf_link_t rf;
static fake_t fh, fr; /* hub chip, remote chip */
static hp_rf_transport *hub_tp;
static hp_rf_link_t hub_link;
static hkey_t hkeys[256];
static int nhkeys;
static hbatt_t hbatt[32];
static hinfo_t hinfo[32];
static int nhbatt, nhinfo, nhother, hord; /* hord: arrival order of battery / remote_info objects */

/* wait() recorder: what the loop select()s on and for how long */
static int rec_n, rec_fd[3];
static unsigned rec_ms;
static int rec_wait(void *c, const int *fds, int n, unsigned ms, int *ready) {
  rec_n = n;
  for (int i = 0; i < n && i < 3; i++) rec_fd[i] = fds[i];
  rec_ms = ms;
  return fk_wait(c, fds, n, ms, ready);
}

static void hub_deliver(void *ctx, const hp_rf_msg_t *m) {
  (void)ctx;
  hp_rf_val_t v[3];
  if (m->type == HP_MSG_KEY_EVENT && hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0) {
    if (nhkeys < 256) hkeys[nhkeys++] = (hkey_t){(uint8_t)v[0].u, (uint8_t)v[1].u, (uint8_t)v[2].u};
  } else if (m->type == HP_MSG_BATTERY && hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0) {
    if (nhbatt < 32) hbatt[nhbatt++] = (hbatt_t){(uint8_t)v[0].u, (uint8_t)v[1].u, (uint16_t)v[2].u, fk.now, hord++};
  } else if (m->type == HP_MSG_REMOTE_INFO) {
    hp_rf_val_t r[6];
    CT_EQ(hp_rf_unpack(m->type, m->payload, m->len, r, 6), 0);
    if (nhinfo < 32)
      hinfo[nhinfo++] = (hinfo_t){(uint8_t)r[0].u, (uint8_t)r[1].u, (uint8_t)r[2].u, (uint8_t)r[3].u, (uint8_t)r[4].u,
                                  (uint8_t)r[5].u, fk.now, hord++};
  } else {
    nhother++;
  }
}

static void hub_pump(void) { (void)hp_rf_link_pump(&hub_link, (uint32_t)fk.now); }

static int hub_send(hp_msg_type_t t, const hp_rf_val_t *v, size_t nv) {
  uint8_t pl[HP_RF_MSG_MAX];
  size_t n;
  if (hp_rf_pack(t, v, nv, pl, sizeof pl, &n) < 0) return -1;
  return hp_rf_link_send(&hub_link, t, pl, n, (uint32_t)fk.now);
}
static int hub_push_page(const uint8_t *d, size_t n) {
  hp_rf_val_t v[3] = {{.u = 1}, {.u = d[1]}, {.p = d, .len = n}};
  return hub_send(HP_MSG_PAGE_PUSH, v, 3);
}

static void ms_sleep(int ms) {
  struct timespec ts = {0, ms * 1000000L};
  nanosleep(&ts, NULL);
}

/* Both radios' reader threads learn their slot from the link-up reports (real time). Wait for that without
 * driving the app: its first uplink must not be sent before the slot is known. */
static void wait_slots(void) {
  for (int i = 0; i < 3000 && (hp_rf_spi_slot(rf.tp) != 1 || hp_rf_spi_slot(hub_tp) != 2); i++) ms_sleep(1);
}

static void settle(int rounds) {
  for (int i = 0; i < rounds; i++) {
    fh.now = fr.now = fk.now;
    hub_pump();
    app_step(&app, APP_LINK_IDLE_WAIT_MS);
    ms_sleep(1);
  }
}
#define WAIT_FOR(cond) \
  do { int _i; for (_i = 0; _i < 3000 && !(cond); _i++) settle(1); if (_i == 3000) { CT_CHECK(cond); } } while (0)
/* every object either side has queued is acked */
#define DRAIN() WAIT_FOR(hp_rf_link_status(&rf.link).queued == 0 && hp_rf_link_status(&hub_link).queued == 0)

static void tick(uint64_t ms) {
  fk.now += ms;
  settle(3);
}

/* The radios' link-up reports (bench, docs/hardware/hub-rf-cc2544.md): neither side is given a slot,
 * both learn it from these. Queued before the open: the reader picks them up first. */
static int no_remote_up; /* the remote's chip never reports link status */
static void radios_link_up(void) {
  const uint8_t hub_up[7] = {0x10, 0x02, 0x41, 0x04, 0x87, 0x21, 0x40};
  const uint8_t rem_up[7] = {0x10, 0x01, 0x41, 0x04, 0x07, 0x0B, 0x88};
  feed(&fh, hub_up, 7);
  if (!no_remote_up) feed(&fr, rem_up, 7);
}

static int setup_ex(int hal, int soc_missing) {
  fk_init(&fk);
  if (soc_missing) fk_set_file(&fk, RA_PATH_SOC, NULL);
  fake_init(&fh, 0);
  fake_init(&fr, 1);
  fh.peer = &fr;
  fr.peer = &fh;
  fr.hal = hal;
  nhkeys = nhbatt = nhinfo = nhother = hord = 0;
  rec_n = 0;
  hp_rf_spi_ops_t ho = fake_ops(&fh);
  hp_rf_spi_cfg_t hc = {NULL, 0, &ho, HP_RF_SPI_ROLE_HUB, 0, false, NULL, NULL, NULL};
  radios_link_up();
  CT_EQ(hp_rf_spi_open(&hc, &hub_tp), 0);
  CT_EQ(hub_tp->claim(hub_tp->self), 0);
  hp_rf_link_init(&hub_link, hub_tp, false, 1, hub_deliver, NULL);

  ra_ops_t ops = fk_ops(&fk);
  ops.wait = rec_wait;
  rf_link_init(&rf, &ops, RF_LINK_REMOTE_ID);
  app_link_t link = rf_link_app(&rf);
  CT_EQ(app_init(&app, &ops, &link, NULL), 0);
  rf_link_set_store(&rf, &app.store); /* as main.c does */
  hp_rf_spi_ops_t ro = fake_ops(&fr);
  hp_rf_spi_cfg_t rc = {NULL, 0, &ro, HP_RF_SPI_ROLE_REMOTE, 0, false, NULL, NULL, NULL};
  int orc = rf_link_open(&rf, &rc);
  if (orc == 0 && !no_remote_up) { /* both radios have learned their slot from the link-up reports before anything is driven */
    wait_slots();
    CT_EQ(hp_rf_spi_slot(rf.tp), 1);
    CT_EQ(hp_rf_spi_slot(hub_tp), 2);
  }
  return orc;
}
/* open, and wait until the remote has spoken: the hub only then knows it is attached */
static void setup(void) {
  CT_EQ(setup_ex(0, 0), 0);
  WAIT_FOR(nhbatt == 1);
}

static void teardown(void) {
  rf_link_close(&rf);
  app_destroy(&app);
  hp_rf_spi_destroy(hub_tp);
}

/* ---- vectors ---- */

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
  size_t n = strlen(s) / 2;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; i++) {
    unsigned b;
    sscanf(s + 2 * i, "%2x", &b);
    out[i] = (uint8_t)b;
  }
  return n;
}
static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *buf = malloc(1 << 17);
  size_t n = fread(buf, 1, (1 << 17) - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}
static uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}
static uint64_t fb_hash(void) {
  memset(fk.fb, 0xA5, sizeof fk.fb); /* same recipe as test_remote_render: sentinel, full repaint */
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(app.display.disp);
  return fnv1a(fk.fb, 480 * 320);
}

/* Golden page `name` from pages.json: its encoded bytes and the golden render hash. */
static int golden(const char *name, uint8_t *raw, size_t *n, char *hash) {
  char *pt = slurp(HP_PAGES_JSON), *gt = slurp(HP_RENDER_JSON);
  cJSON *pages = pt ? cJSON_Parse(pt) : NULL, *gold = gt ? cJSON_Parse(gt) : NULL;
  int ok = 0;
  const cJSON *v;
  if (pages && gold)
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(pages, "valid")) {
      if (strcmp(cJSON_GetObjectItem(v, "name")->valuestring, name)) continue;
      *n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, 300);
      snprintf(hash, 20, "%s", cJSON_GetObjectItem(cJSON_GetObjectItem(gold, "pages"), name)->valuestring);
      ok = 1;
    }
  cJSON_Delete(pages);
  cJSON_Delete(gold);
  free(pt);
  free(gt);
  return ok;
}

/* page 2, 4x6: tile id 5 at (1,1) 2x1 (as test_remote_app) */
static const uint8_t PAGE2[] = {0x01, 0x02, 0x04, 0x06, 0x01, 0x01, 0x05, 0x11, 0x21, 0x00};

/* ---- tests ---- */

static void test_page_push_renders_golden(void) {
  uint8_t raw[300];
  size_t n;
  char hash[20], got[20];
  CT_CHECK(golden("one_button", raw, &n, hash));
  setup();
  WAIT_FOR(nhbatt == 1); /* the remote spoke first: the hub knows it is there */
  CT_EQ(hub_push_page(raw, n), 0);
  WAIT_FOR(app.ui.page.page_id == 1);
  DRAIN();
  /* the golden has an unknown battery and a live link: the status bar here shows the fake gauge, so reset it */
  ra_ui_set_battery(&app.ui, -1, 0);
  ra_ui_set_link(&app.ui, 0);
  lv_refr_now(app.display.disp);
  snprintf(got, sizeof got, "%016llx", (unsigned long long)fb_hash());
  CT_CHECK(!strcmp(got, hash));
  if (strcmp(got, hash)) fprintf(stderr, "render %s golden %s\n", got, hash);
  CT_CHECK(!app.ui.fallback);
  teardown();
}

/* ---- theme chain: compiled config (tests/vectors/theme_chain.json, made by tests/py/test_theme_chain.py) -> hub link -> radio
 * -> remote -> framebuffer, against rendering the same theme and page directly ---- */
/* The hub's frames reach the remote on its polls, which follow the fake clock: advance it, not just the threads. */
#define TICK_FOR(cond) \
  do { int _i; for (_i = 0; _i < 600 && !(cond); _i++) tick(50); CT_CHECK(cond); } while (0)
#ifndef HP_THEME_CHAIN_JSON
#error "HP_THEME_CHAIN_JSON must be defined"
#endif
static struct {
  uint8_t gen, theme[HP_THEME_BYTES], pages[8][HP_PAGE_MAX_BYTES];
  size_t plen[8];
  int n;
} chain;

static void chain_load(void) {
  char *t = slurp(HP_THEME_CHAIN_JSON);
  cJSON *j = t ? cJSON_Parse(t) : NULL;
  CT_CHECK(j != NULL);
  if (!j) return;
  chain.gen = (uint8_t)cJSON_GetObjectItem(j, "gen")->valueint;
  CT_EQ(unhex(cJSON_GetObjectItem(j, "theme")->valuestring, chain.theme, sizeof chain.theme), (size_t)HP_THEME_BYTES);
  const cJSON *p;
  chain.n = 0;
  cJSON_ArrayForEach(p, cJSON_GetObjectItem(j, "pages")) {
    chain.plen[chain.n] = unhex(p->valuestring, chain.pages[chain.n], HP_PAGE_MAX_BYTES);
    chain.n++;
  }
  cJSON_Delete(j);
  free(t);
}

static int hub_push_theme(void) {
  hp_rf_val_t v[3] = {{.u = 1}, {.u = chain.gen}, {.p = chain.theme, .len = HP_THEME_BYTES}};
  return hub_send(HP_MSG_THEME_PUSH, v, 3);
}

static void status_bar_neutral(void) { /* as the page golden: the fake gauge and link state out of the picture */
  ra_ui_set_battery(&app.ui, -1, 0);
  ra_ui_set_link(&app.ui, 0);
}

static uint64_t chain_direct_render(int page) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), 0);
  CT_EQ(ra_ui_set_custom_theme(chain.theme, HP_THEME_BYTES), 0);
  hp_page_t p;
  CT_EQ(hp_page_decode(chain.pages[page], chain.plen[page], &p), 0);
  ra_ui_show_page(&app.ui, &p);
  status_bar_neutral();
  uint64_t h = fb_hash();
  app_destroy(&app);
  return h;
}

static void test_theme_chain_renders_like_a_direct_render(void) {
  chain_load();
  CT_CHECK(chain.n >= 1);
  setup();
  CT_EQ(hub_push_theme(), 0); /* the theme first, as the hub orders it */
  for (int i = 0; i < chain.n; i++) {
    hp_rf_val_t v[3] = {{.u = 1}, {.u = i}, {.p = chain.pages[i], .len = chain.plen[i]}};
    CT_EQ(hub_send(HP_MSG_PAGE_PUSH, v, 3), 0);
  }
  TICK_FOR(hp_store_complete(&app.store) && app.cur >= 0 && app.ui.page.page_id == app.cur);
  TICK_FOR(hp_rf_link_status(&rf.link).queued == 0 && hp_rf_link_status(&hub_link).queued == 0);
  CT_EQ(ra_theme_cur()->id, RA_THEME_CUSTOM_ID);
  int cur = app.cur;
  status_bar_neutral();
  uint64_t chained = fb_hash();
  /* the remote reported the complete set only now */
  TICK_FOR(nhinfo >= 2 && hinfo[nhinfo - 1].gen == chain.gen && hinfo[nhinfo - 1].pages == chain.n);
  teardown();
  uint64_t direct = chain_direct_render(cur);
  CT_CHECK(chained == direct);
  if (chained != direct) fprintf(stderr, "chain %016llx direct %016llx\n", (unsigned long long)chained, (unsigned long long)direct);
}

/* pages first: the remote reports no set and shows none until the late theme arrives, then both follow */
static void test_theme_chain_late_theme_completes_the_set(void) {
  chain_load();
  setup();
  for (int i = 0; i < chain.n; i++) {
    hp_rf_val_t v[3] = {{.u = 1}, {.u = i}, {.p = chain.pages[i], .len = chain.plen[i]}};
    CT_EQ(hub_send(HP_MSG_PAGE_PUSH, v, 3), 0);
  }
  TICK_FOR(hp_store_count(&app.store) == (unsigned)chain.n);
  TICK_FOR(hp_rf_link_status(&rf.link).queued == 0 && hp_rf_link_status(&hub_link).queued == 0);
  tick(RF_LINK_RETRY_MS);
  CT_CHECK(!hp_store_complete(&app.store));
  CT_CHECK(!(app.ui.page.version == 2 && !app.ui.recv_total));
  for (int i = 0; i < nhinfo; i++) CT_EQ(hinfo[i].pages, 0); /* never reported as held */
  CT_EQ(hub_push_theme(), 0);
  TICK_FOR(hp_store_complete(&app.store) && app.ui.page.version == 2 && !app.ui.recv_total);
  TICK_FOR(hinfo[nhinfo - 1].gen == chain.gen && hinfo[nhinfo - 1].pages == chain.n);
  teardown();
}

/* a theme that is not valid changes nothing */
static void test_theme_chain_corrupt_theme_is_ignored(void) {
  chain_load();
  setup();
  uint8_t bad[HP_THEME_BYTES];
  memcpy(bad, chain.theme, sizeof bad);
  bad[0] = 0;
  hp_rf_val_t v[3] = {{.u = 1}, {.u = chain.gen}, {.p = bad, .len = sizeof bad}};
  CT_EQ(hub_send(HP_MSG_THEME_PUSH, v, 3), 0);
  TICK_FOR(hp_rf_link_status(&rf.link).queued == 0 && hp_rf_link_status(&hub_link).queued == 0);
  tick(200);
  CT_CHECK(!app.store.have_theme);
  teardown();
}

/* compat (docs/proto/compat.md): a hub->remote message type the remote does not know (what an old remote makes of theme_push) is
 * acked and dropped; the link and the next page are unaffected. Code 29 is unassigned. */
static void test_unknown_hub_message_type_is_acked_and_ignored(void) {
  setup();
  const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CT_EQ(hp_rf_link_send(&hub_link, (hp_msg_type_t)29, junk, sizeof junk, (uint32_t)fk.now), 0);
  TICK_FOR(hp_rf_link_status(&hub_link).queued == 0);
  CT_EQ(hp_rf_link_status(&hub_link).failures, 0u);
  CT_CHECK(!app.ui.fallback);
  uint8_t raw[300];
  size_t n;
  char hash[20];
  CT_CHECK(golden("one_button", raw, &n, hash));
  CT_EQ(hub_push_page(raw, n), 0);
  TICK_FOR(app.ui.page.page_id == 1);
  teardown();
}

static void test_keys_and_taps_reach_the_hub(void) {
  uint8_t raw[300];
  size_t n;
  char hash[20];
  CT_CHECK(golden("one_button", raw, &n, hash));
  setup();
  CT_EQ(hub_push_page(raw, n), 0);
  WAIT_FOR(app.ui.page.page_id == 1);
  DRAIN();
  int base = nhkeys;
  fk_key(&fk, 0, 115, 1); /* VolumeUp */
  tick(50);
  fk_key(&fk, 0, 115, 0);
  tick(50);
  WAIT_FOR(nhkeys == base + 2);
  tick(400);
  CT_EQ(nhkeys, base + 2); /* exactly one event per action */
  uint8_t vol = hp_hwkey_by_name("VolumeUp")->id;
  CT_EQ(hkeys[base].remote, RF_LINK_REMOTE_ID);
  CT_EQ(hkeys[base].key, vol);
  CT_EQ(hkeys[base].action, HP_KEY_ACTION_PRESS);
  CT_EQ(hkeys[base + 1].key, vol);
  CT_EQ(hkeys[base + 1].action, HP_KEY_ACTION_RELEASE);
  /* a tap on the page's button (id 1) */
  fk_touch(&fk, 10, 42, 1);
  tick(50);
  fk_touch(&fk, 10, 42, 0);
  tick(50);
  WAIT_FOR(nhkeys == base + 4);
  CT_EQ(hkeys[base + 2].key, HP_SOFT_KEY_BASE + 1);
  CT_EQ(hkeys[base + 2].action, HP_KEY_ACTION_PRESS);
  CT_EQ(hkeys[base + 3].key, HP_SOFT_KEY_BASE + 1);
  CT_EQ(hkeys[base + 3].action, HP_KEY_ACTION_RELEASE);
  CT_CHECK(!app.ui.fallback);
  teardown();
}

/* Message types added after this firmware (tile_state, remote_info) are dropped without effect. */
static void test_tile_state_applies_and_unknown_types_are_dropped(void) {
  setup();
  CT_EQ(hub_push_page(PAGE2, sizeof PAGE2), 0);
  WAIT_FOR(app.ui.page.page_id == 2);
  uint64_t before = fb_hash();
  hp_rf_val_t r[6] = {{.u = 1}, {.u = 2}, {.u = 3}, {.u = 50}, {.u = 0}, {.u = 0}};
  CT_EQ(hub_send(HP_MSG_REMOTE_INFO, r, 6), 0); /* a type the remote does not act on: dropped, not an error */
  hp_rf_val_t t[5] = {{.u = 1}, {.u = 2}, {.u = 5}, {.u = 2}, {.p = (const uint8_t *)"On", .len = 2}};
  CT_EQ(hub_send(HP_MSG_TILE_STATE, t, 5), 0);
  hp_rf_val_t s[2] = {{.u = 3}, {.u = 1}};
  CT_EQ(hub_send(HP_MSG_STATE_UPDATE, s, 2), 0); /* a later message still arrives, in order */
  WAIT_FOR(app.state_updates == 1);
  DRAIN();
  CT_CHECK(!strcmp(app.ui.page.widgets[0].text, "On")); /* a version 1 page takes the text of a tile_state */
  CT_CHECK(fb_hash() != before);
  CT_CHECK(!app.ui.fallback);
  teardown();
}

/* remote_info is the first object after link-up, ahead of the battery message, and carries what the remote can do. */
static void test_remote_info_is_first_uplink(void) {
  setup();
  WAIT_FOR(nhinfo >= 1 && nhbatt >= 1);
  CT_EQ(hinfo[0].remote, RF_LINK_REMOTE_ID);
  CT_EQ(hinfo[0].fmt, 2);
  CT_EQ(hinfo[0].caps, 15);
  CT_EQ(hinfo[0].icons, RA_ICON_MAX);
  CT_CHECK(hinfo[0].ord < hbatt[0].ord);
  CT_EQ(nhinfo, 1);
  teardown();
}

/* A v2 page `id` of `total`: one button (the bytes of test_link_v2.c's mkpage). */
static size_t v2raw(uint8_t *o, uint8_t id, uint8_t total, uint8_t gen) {
  const uint8_t b[] = {2, id, 2, 2, 1, gen, total, 0, 0, 0, 0, 0, 1, 0x00, 0x11, 1, 'X'};
  memcpy(o, b, sizeof b);
  return sizeof b;
}

/* remote_info says what the page store holds, so the hub (and Home Assistant) can skip a push that changes nothing:
 * empty at the first announcement, then the new set once it is complete (one more message, not one per page). */
static void test_remote_info_reports_the_page_store(void) {
  setup();
  WAIT_FOR(nhinfo >= 1);
  CT_EQ(hinfo[0].gen, 0);
  CT_EQ(hinfo[0].pages, 0);
  uint8_t raw[300];
  size_t n = v2raw(raw, 0, 2, 0xA7);
  CT_EQ(hub_push_page(raw, n), 0);
  WAIT_FOR(app.store.count == 1);
  DRAIN();
  tick(200);
  CT_EQ(nhinfo, 1); /* a partial set is not announced */
  n = v2raw(raw, 1, 2, 0xA7);
  CT_EQ(hub_push_page(raw, n), 0);
  WAIT_FOR(nhinfo == 2);
  CT_EQ(hinfo[1].gen, 0xA7);
  CT_EQ(hinfo[1].pages, 2);
  tick(400);
  CT_EQ(nhinfo, 2); /* announced once */
  teardown();
}

static void test_remote_info_reports_a_store_it_already_holds(void) {
  fk_init(&fk);
  fake_init(&fh, 0);
  fake_init(&fr, 1);
  fh.peer = &fr;
  fr.peer = &fh;
  nhkeys = nhbatt = nhinfo = nhother = hord = 0;
  hp_rf_spi_ops_t ho = fake_ops(&fh);
  hp_rf_spi_cfg_t hc = {NULL, 0, &ho, HP_RF_SPI_ROLE_HUB, 0, false, NULL, NULL, NULL};
  radios_link_up();
  CT_EQ(hp_rf_spi_open(&hc, &hub_tp), 0);
  CT_EQ(hub_tp->claim(hub_tp->self), 0);
  hp_rf_link_init(&hub_link, hub_tp, false, 1, hub_deliver, NULL);
  ra_ops_t ops = fk_ops(&fk);
  ops.wait = rec_wait;
  rf_link_init(&rf, &ops, RF_LINK_REMOTE_ID);
  app_link_t link = rf_link_app(&rf);
  CT_EQ(app_init(&app, &ops, &link, NULL), 0);
  rf_link_set_store(&rf, &app.store);
  uint8_t raw[300];
  size_t n = v2raw(raw, 0, 1, 0x3C);
  CT_EQ(hp_store_put(&app.store, raw, n), HP_STORE_OK); /* a set kept across the link, as after a hub restart */
  hp_rf_spi_ops_t ro = fake_ops(&fr);
  hp_rf_spi_cfg_t rc = {NULL, 0, &ro, HP_RF_SPI_ROLE_REMOTE, 0, false, NULL, NULL, NULL};
  CT_EQ(rf_link_open(&rf, &rc), 0);
  wait_slots(); /* before anything is driven: a send before the slot is learned is refused and retried only 5 s later on the (frozen) fake clock */
  CT_EQ(hp_rf_spi_slot(rf.tp), 1);
  CT_EQ(hp_rf_spi_slot(hub_tp), 2);
  WAIT_FOR(nhinfo >= 1);
  CT_EQ(hinfo[0].gen, 0x3C);
  CT_EQ(hinfo[0].pages, 1);
  teardown();
}

static void test_tile_update_changes_render(void) {
  setup();
  CT_EQ(hub_push_page(PAGE2, sizeof PAGE2), 0);
  WAIT_FOR(app.ui.page.page_id == 2);
  uint64_t before = fb_hash();
  hp_rf_val_t v[4] = {{.u = 1}, {.u = 2}, {.u = 5}, {.p = (const uint8_t *)"72 F", .len = 4}};
  CT_EQ(hub_send(HP_MSG_TILE_UPDATE, v, 4), 0);
  WAIT_FOR(!strcmp(app.ui.page.widgets[0].text, "72 F"));
  DRAIN();
  CT_CHECK(fb_hash() != before);
  /* a state_update is consumed without any UI side effect */
  hp_rf_val_t s[2] = {{.u = 3}, {.u = 1}};
  CT_EQ(hub_send(HP_MSG_STATE_UPDATE, s, 2), 0);
  WAIT_FOR(app.state_updates == 1);
  CT_EQ(app.last_activity, 3);
  CT_CHECK(app.last_power);
  CT_CHECK(!app.ui.fallback);
  teardown();
}

static void test_dropped_ack_delivers_once(void) {
  setup();
  WAIT_FOR(nhbatt == 1);
  DRAIN();
  int base = nhkeys, w0 = fr.nw;
  fh.drop_acks = 1; /* the hub's ack for the press is lost */
  fk_key(&fk, 0, 115, 1);
  tick(10);
  WAIT_FOR(nhkeys == base + 1);
  tick(160); /* past the 150 ms retry: the press is sent again */
  DRAIN();
  CT_EQ(nhkeys, base + 1);          /* the duplicate was acked, not delivered */
  CT_EQ(fr.nw - w0, 2);             /* sent twice */
  CT_CHECK(!app.ui.fallback);
  fk_key(&fk, 0, 115, 0);
  tick(10);
  WAIT_FOR(nhkeys == base + 2);
  teardown();
}

static void test_peer_that_never_acks_shows_fallback_only(void) {
  setup();
  WAIT_FOR(nhbatt == 1);
  DRAIN();
  CT_EQ(hub_push_page(PAGE2, sizeof PAGE2), 0);
  WAIT_FOR(app.ui.page.page_id == 2);
  DRAIN();
  int keys = nhkeys, batts = nhbatt;
  fr.drop_all = 1; /* nothing the remote says reaches the hub */
  fk_key(&fk, 0, 115, 1);
  tick(10);
  CT_CHECK(!app.ui.fallback); /* retries are still running */
  CT_CHECK(rec_ms <= 150);    /* the link's retry deadline bounds the wait */
  for (int i = 0; i < 4; i++) tick(160);
  CT_CHECK(!app.ui.fallback); /* one failed object is not enough... */
  for (int k = 0; k < 2; k++) { /* ...it takes three in a row, and 5 s without hearing the hub */
    fk_key(&fk, 0, 115, 0);
    for (int i = 0; i < 5; i++) tick(1100);
    fk_key(&fk, 0, 115, 1);
    for (int i = 0; i < 4; i++) tick(160);
  }
  CT_CHECK(app.ui.fallback);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_FALLBACK);
  CT_EQ(nhkeys, keys); /* nothing got through, and nothing else was delivered */
  CT_EQ(nhbatt, batts);
  CT_EQ(nhother, 0);
  /* the hub comes back: a pushed page replaces the fallback */
  fr.drop_all = 0;
  CT_EQ(hub_push_page(PAGE2, sizeof PAGE2), 0);
  WAIT_FOR(!app.ui.fallback);
  CT_EQ(app.ui.page.page_id, 2);
  teardown();
}

static void test_idle_link_is_silent_and_heartbeats(void) {
  setup();
  WAIT_FOR(nhbatt == 1); /* right after the link opens */
  CT_EQ(hbatt[0].remote, RF_LINK_REMOTE_ID);
  CT_EQ(hbatt[0].pct, 100);
  CT_EQ(hbatt[0].mv, 0);
  uint64_t t0 = fk.now;
  CT_EQ(hub_push_page(PAGE2, sizeof PAGE2), 0);
  WAIT_FOR(app.ui.page.page_id == 2);
  DRAIN();
  fk_set_file(&fk, RA_PATH_SOC, "42.4\n");
  int keys = nhkeys;
  for (int s = 1; s <= 601; s++) { /* ten minutes of nobody touching anything */
    fk.now = t0 + (uint64_t)s * 1000;
    settle(1);
    if (hp_rf_link_status(&rf.link).queued) DRAIN();
    CT_CHECK(!app.ui.fallback);
  }
  CT_EQ(app.ui.page.page_id, 2);
  CT_EQ(nhkeys, keys);
  CT_EQ(nhbatt, 3); /* open, +5 min, +10 min */
  CT_EQ(nhinfo, 3); /* remote_info rides with each heartbeat: the hub only relays a change */
  CT_EQ(hbatt[1].pct, 42);
  CT_EQ(hbatt[1].at - t0, RF_LINK_BATTERY_MS);
  CT_EQ(hbatt[2].at - t0, 2 * RF_LINK_BATTERY_MS);
  CT_EQ(nhother, 0);
  teardown();
}

/* inner kind of each frame the remote wrote */
static int count_remote_writes(uint8_t kind) {
  int n = 0;
  for (int i = 0; i < fr.nw && i < 512; i++)
    if (fr.w[i].len == 32 && fr.w[i].head[4] == kind) n++;
  return n;
}

static void test_polls_only_while_awake(void) {
  setup();
  WAIT_FOR(nhbatt == 1);
  DRAIN();
  int p0 = count_remote_writes(HP_RF_KIND_POLL);
  for (int i = 0; i < 20; i++) tick(RF_LINK_IDLE_POLL_MS); /* 5 s awake and idle */
  int awake_polls = count_remote_writes(HP_RF_KIND_POLL) - p0;
  CT_CHECK(awake_polls >= 15 && awake_polls <= 22);
  CT_EQ(nhother, 0); /* the hub ignored every poll: no delivery, no ack */
  CT_EQ(hp_rf_link_status(&hub_link).delivered, 2u); /* remote_info + battery */
  for (int i = 0; i < 40; i++) tick(1000); /* idle past the backlight-off time */
  CT_EQ(app.power.state, RA_PW_OFF);
  int p1 = count_remote_writes(HP_RF_KIND_POLL);
  for (int i = 0; i < 20; i++) tick(1000); /* asleep: silence */
  CT_EQ(count_remote_writes(HP_RF_KIND_POLL), p1);
  fk_key(&fk, 0, 115, 1); /* wakes; the press itself is consumed */
  for (int i = 0; i < 4; i++) tick(RF_LINK_IDLE_POLL_MS);
  CT_CHECK(count_remote_writes(HP_RF_KIND_POLL) > p1);
  teardown();
}

static void test_heartbeat_unreadable_soc(void) {
  /* never had a reading: nothing honest to send, retried every 5 s */
  CT_EQ(setup_ex(0, 1), 0);
  for (int i = 0; i < 4; i++) tick(RF_LINK_RETRY_MS);
  CT_EQ(nhbatt, 0);
  CT_CHECK(!app.ui.fallback);
  fk_set_file(&fk, RA_PATH_SOC, "88.0\n");
  tick(RF_LINK_RETRY_MS);
  WAIT_FOR(nhbatt == 1);
  CT_EQ(hbatt[0].pct, 88);
  uint64_t sent = hbatt[0].at;
  /* a later failed read repeats the last good value rather than going silent */
  fk_set_file(&fk, RA_PATH_SOC, "garbage\n");
  while (fk.now < sent + RF_LINK_BATTERY_MS) tick(10000);
  WAIT_FOR(nhbatt == 2);
  CT_EQ(hbatt[1].pct, 88);
  fk_set_file(&fk, RA_PATH_SOC, NULL);
  while (fk.now < hbatt[1].at + RF_LINK_BATTERY_MS) tick(10000);
  WAIT_FOR(nhbatt == 3);
  CT_EQ(hbatt[2].pct, 88);
  CT_CHECK(!app.ui.fallback);
  teardown();
}

static void test_wait_includes_link_fd_and_deadline(void) {
  setup();
  WAIT_FOR(nhbatt == 1);
  DRAIN();
  for (int i = 0; i < 3; i++) tick(100); /* let LVGL's refresh timer go idle */
  CT_EQ(rec_n, 3);
  CT_EQ(rec_fd[2], hp_rf_spi_wake_fd(rf.tp));
  CT_CHECK(rec_fd[2] >= 0);
  CT_CHECK(rec_ms <= RF_LINK_IDLE_POLL_MS); /* idle: woken for the next poll, never later */
  fr.drop_all = 1;
  fk_key(&fk, 0, 115, 1);
  tick(1);
  CT_CHECK(rec_ms <= 150); /* an in-flight object's retry deadline bounds it */
  teardown();
}

static void test_open_refused_when_hal_alive(void) {
  CT_EQ(setup_ex(1, 0), -HP_E_HALBUSY);
  CT_EQ(fr.nw, 0);
  CT_EQ(fr.nopens, 0);
  settle(2);
  CT_CHECK(app.ui.fallback); /* no radio: say so, keep running */
  CT_EQ(rec_n, 2);           /* and nothing to select() on */
  fk_key(&fk, 0, 115, 1);
  settle(2); /* a key with no link must not crash */
  teardown();
}

static void test_cli(void) {
  ra_cli_t c;
  char *a1[] = {"x", NULL};
  CT_EQ(ra_cli_parse(1, a1, &c), 0);
  CT_EQ(c.rf_spi, 0); /* host default: the null link */
  CT_EQ(c.powermode, 0);
  CT_EQ(c.slot, 0);
  char *a2[] = {"x", "--rf", "spi", "--powermode", "0x30", "--slot", "3"};
  CT_EQ(ra_cli_parse(7, a2, &c), 0);
  CT_CHECK(c.rf_spi);
  CT_EQ(c.powermode, 0x30);
  CT_EQ(c.slot, 3);
  char *a3[] = {"x", "--rf=none", "--powermode=0xF0", "--slot=15"};
  CT_EQ(ra_cli_parse(4, a3, &c), 0);
  CT_CHECK(!c.rf_spi);
  CT_EQ(c.powermode, 0xF0);
  CT_EQ(c.slot, 15);
  char *a4[] = {"x", "--powermode", "0"};
  CT_EQ(ra_cli_parse(3, a4, &c), 0);
  CT_EQ(c.powermode, 0);
  const char *bad[][3] = {
      {"--rf", "usb", NULL},        {"--rf", "", NULL},         {"--rf", NULL, NULL},       {"--powermode", "0x05", NULL},
      {"--powermode", "0x100", NULL}, {"--powermode", "-16", NULL}, {"--powermode", "zz", NULL}, {"--powermode", "0x3g", NULL},
      {"--slot", "16", NULL}, {"--slot", "0", NULL}, {"--slot=0", NULL, NULL},       {"--slot", "-1", NULL},     {"--slot", "", NULL},       {"--slot", "1x", NULL},
      {"--slot", NULL, NULL},       {"--bogus", "1", NULL},     {"rf", "spi", NULL},
  };
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *av[4] = {"x", (char *)bad[i][0], (char *)bad[i][1], NULL};
    CT_EQ(ra_cli_parse(bad[i][1] ? 3 : 2, av, &c), -1);
  }
  CT_CHECK(!c.rf_debug);
  char *d1[] = {"x", "--rf-debug"};
  CT_EQ(ra_cli_parse(2, d1, &c), 0);
  CT_CHECK(c.rf_debug);
  char *d2[] = {"x", "--slot", "3", "--rf-debug", "--powermode", "0x10"};
  CT_EQ(ra_cli_parse(6, d2, &c), 0);
  CT_CHECK(c.rf_debug);
  CT_EQ(c.slot, 3);
  CT_EQ(c.powermode, 0x10);
  CT_CHECK(!c.ui_debug);
  char *u1[] = {"x", "--ui-debug"};
  CT_EQ(ra_cli_parse(2, u1, &c), 0);
  CT_CHECK(c.ui_debug);
  char *u2[] = {"x", "--ui-debug", "--slot", "1", "--rf-debug"};
  CT_EQ(ra_cli_parse(5, u2, &c), 0);
  CT_CHECK(c.ui_debug && c.rf_debug);
  char *u3[] = {"x", "--ui-debug=1"};
  CT_EQ(ra_cli_parse(2, u3, &c), -1); /* a flag takes no value */
  char *d3[] = {"x", "--rf-debug=1"};
  CT_EQ(ra_cli_parse(2, d3, &c), -1); /* a flag takes no value */
  char *a5[] = {"x", "--slot", "2", "--rf", "bad"};
  ra_cli_t keep = {.slot = 9};
  c = keep;
  CT_EQ(ra_cli_parse(5, a5, &c), -1);
  CT_EQ(c.slot, 9); /* untouched on error */
}

static void test_shutdown_destroys_rfspi_once(void) {
  setup();
  WAIT_FOR(nhbatt == 1);
  DRAIN();
  CT_EQ(fr.nclosed, 0);
  rf_link_close(&rf);
  CT_EQ(fr.nclosed, 1);
  CT_CHECK(rf.tp == NULL);
  rf_link_close(&rf); /* idempotent */
  CT_EQ(fr.nclosed, 1);
  app_destroy(&app); /* the app does not own the radio */
  CT_EQ(fr.nclosed, 1);
  hp_rf_spi_destroy(hub_tp);
  CT_EQ(fh.nclosed, 1);
  /* the app path: main() stops the loop, closes the link, then the app */
  CT_EQ(setup_ex(0, 0), 0);
  WAIT_FOR(nhbatt == 1);
  teardown();
  CT_EQ(fr.nclosed, 1);
}

static const uint8_t CMD_INIT[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};

/* The RF seam: one open of /dev/rfspi, O_RDWR; only the init command (7 bytes) and 32-byte remote
 * frames are ever written; never anything longer, never another device. */
static int rf_log_is_safe(const fake_t *f) {
  int ok = 1;
  if (f->nopens != 1 || strcmp(f->opens[0], "/dev/rfspi") || f->open_flags[0] != O_RDWR) {
    fprintf(stderr, "unsafe rf open (%d opens, first %s)\n", f->nopens, f->opens[0]);
    ok = 0;
  }
  if (f->max_write > 32) { fprintf(stderr, "write of %zu bytes\n", f->max_write); ok = 0; }
  int n = f->nw < 512 ? f->nw : 512;
  for (int i = 0; i < n; i++) {
    int init = f->w[i].len == 7 && !memcmp(f->w[i].head, CMD_INIT, 7);
    int frame = f->w[i].len == 32 && f->w[i].b0 == 0x20 && f->w[i].b2 == 0x12;
    if (!init && !frame) { fprintf(stderr, "unexpected rf write %d: len %zu b0 %02x\n", i, f->w[i].len, f->w[i].b0); ok = 0; }
  }
  return ok;
}

static void test_full_session_is_safe_with_rf(void) {
  uint8_t raw[300];
  size_t n;
  char hash[20];
  CT_CHECK(golden("one_button", raw, &n, hash));
  setup();
  WAIT_FOR(nhbatt == 1);
  CT_EQ(hub_push_page(raw, n), 0);
  WAIT_FOR(app.ui.page.page_id == 1);
  fk_key(&fk, 0, 115, 1);
  tick(100);
  fk_key(&fk, 0, 115, 0);
  tick(100);
  fk_touch(&fk, 10, 42, 1);
  tick(100);
  fk_touch(&fk, 10, 42, 0);
  tick(100);
  fk_key(&fk, 0, 114, 1); /* a held key: press, holds, release */
  tick(50);
  tick(700);
  fk_key(&fk, 0, 114, 0);
  tick(50);
  WAIT_FOR(nhkeys == 7);
  DRAIN();
  tick(11000); /* dim */
  tick(20000); /* off */
  fk_key(&fk, 0, 115, 1); /* the waking press is consumed, never sent */
  fk_key(&fk, 0, 115, 0);
  tick(100);
  CT_EQ(nhkeys, 7);
  tick(RF_LINK_BATTERY_MS);
  DRAIN();
  CT_CHECK(nhbatt >= 2);
  fk_set_file(&fk, RA_PATH_SOC, "2.0\n"); /* the guard's exact condition: one poweroff */
  fk_set_file(&fk, RA_PATH_POWERSRC, "battery\n");
  tick(60000);
  tick(60000);
  CT_EQ(fk.nexec, 1);

  /* the app seam: the allow-list as ever, and the radio is not its business */
  CT_CHECK(fk_log_is_safe(&fk, NULL));
  const char *never[] = {"rfspi", "rffw", "mspfw", "watchdog", "mtd", "power/state", "kill", "charger", "event1", "event3"};
  for (int i = 0; i < fk.nlog; i++)
    for (unsigned j = 0; j < sizeof never / sizeof never[0]; j++)
      if (strstr(fk.log[i], never[j])) { fprintf(stderr, "forbidden: %s\n", fk.log[i]); CT_CHECK(0); }
  /* the radio seam */
  CT_CHECK(rf_log_is_safe(&fr));
  /* both sys instances (the app's and the link's) refuse the radio and every other forbidden node */
  ra_sys_t *seams[2] = {&app.sys, &rf.sys};
  const char *bad[] = {"/dev/rfspi", "/dev/rffw", "/dev/mspfw", "/dev/watchdog", "/dev/mtd0"};
  int opens = fk_count(&fk, "open");
  for (int s = 0; s < 2; s++)
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      errno = 0;
      CT_EQ(ra_open_input(seams[s], bad[i]), -1);
      CT_EQ(errno, EPERM);
      errno = 0;
      CT_EQ(ra_sysfs_write(seams[s], bad[i], "1"), -1);
      CT_EQ(errno, EPERM);
    }
  CT_EQ(fk_count(&fk, "open"), opens); /* none of it reached the ops table */
  teardown();
  CT_EQ(fr.nclosed, 1);
  CT_CHECK(rf_log_is_safe(&fr));
}

/* ---- rejoin: sync pulse (12 01 00 ...) on events only ---- */

static int count_syncs(void) {
  int n = 0;
  for (int i = 0; i < fr.nw && i < 512; i++)
    if (fr.w[i].len == 10 && fr.w[i].head[0] == 0x12 && fr.w[i].head[1] == 0x01) n++;
  return n;
}
static int chip_attached(void) { return rf.tp->attached(rf.tp->self); }
static void remote_chip_link(int up) {
  const uint8_t up_r[7] = {0x10, 0x01, 0x41, 0x04, 0x07, 0x0B, 0x88};
  const uint8_t lost_r[7] = {0x10, 0x01, 0x41, 0x04, 0x47, 0x0B, 0x88};
  feed(&fr, up ? up_r : lost_r, 7);
  for (int i = 0; i < 3000 && chip_attached() != up; i++) ms_sleep(1);
  CT_EQ(chip_attached(), up);
  settle(3);
}
/* idle for `ms` in 1 s steps, noting activity every 5 s so the screen stays awake without a link event */
static void idle_awake(uint64_t ms) {
  for (uint64_t t = 0; t < ms; t += 1000) {
    if (t % 5000 == 0) ra_power_activity(&app.power, fk.now); /* keeps the screen on; not a link event */
    tick(1000);
  }
}

static void test_rejoin_silent_while_link_up(void) {
  setup();
  idle_awake(20000);
  CT_EQ(count_syncs(), 0);
  teardown();
}

static void test_rejoin_on_loss_then_capped_backoff(void) {
  setup();
  DRAIN();
  fh.drop_all = 1; /* the hub stays unreachable */
  idle_awake(1000);
  CT_EQ(count_syncs(), 0);
  remote_chip_link(0); /* status 0x47 */
  CT_EQ(count_syncs(), 1); /* the loss itself: one immediate attempt */
  idle_awake(1000);
  CT_EQ(count_syncs(), 1);
  idle_awake(1000); /* +2 s */
  CT_EQ(count_syncs(), 2);
  idle_awake(4000); /* +4 s */
  CT_EQ(count_syncs(), 3);
  idle_awake(8000);
  CT_EQ(count_syncs(), 4);
  idle_awake(16000);
  CT_EQ(count_syncs(), 5);
  idle_awake(120000); /* capped: nothing more without an event */
  CT_EQ(count_syncs(), 5);
  remote_chip_link(1); /* up again: re-armed state cleared, no more pulses */
  idle_awake(60000);
  CT_EQ(count_syncs(), 5);
  teardown();
}

static void test_rejoin_on_keypress_while_lost(void) {
  setup();
  DRAIN();
  fh.drop_all = 1;
  remote_chip_link(0);
  idle_awake(40000); /* retries exhausted */
  int base = count_syncs();
  CT_EQ(base, 5);
  fh.drop_all = 1; /* the hub stays unreachable: no ack brings the link back */
  fk_key(&fk, 0, 115, 1);
  tick(50);
  CT_EQ(count_syncs(), base + 1); /* a press re-initiates the link; the key itself still goes out */
  idle_awake(3000); /* and re-arms the backoff */
  CT_EQ(count_syncs(), base + 2);
  teardown();
}

static void test_rejoin_on_wake_and_resume(void) {
  setup();
  DRAIN();
  fh.drop_all = 1; /* the hub stays unreachable */
  remote_chip_link(0);
  int base = count_syncs();
  for (int i = 0; i < 50; i++) tick(1000); /* backlight off: asleep, retries stopped */
  CT_EQ(app.power.state, RA_PW_OFF);
  int quiet = count_syncs();
  CT_CHECK(quiet <= base + 4);
  for (int i = 0; i < 30; i++) tick(1000);
  CT_EQ(count_syncs(), quiet); /* asleep: no activity */
  fk_touch(&fk, 300, 300, 1); /* display wake */
  fk_touch(&fk, 300, 300, 0);
  tick(50);
  CT_EQ(count_syncs(), quiet + 1);
  tick(RF_LINK_SYNC_COALESCE_MS + 10);
  app_link_t lk = rf_link_app(&rf);
  lk.resumed(lk.ctx, fk.now, 60000); /* resume from suspend */
  CT_EQ(count_syncs(), quiet + 2);
  lk.resumed(lk.ctx, fk.now + 10, 60000); /* back to back: coalesced into the one pulse */
  lk.touched(lk.ctx);
  CT_EQ(count_syncs(), quiet + 2);
  remote_chip_link(1);
  lk.resumed(lk.ctx, fk.now, 60000); /* link is up: resume sends nothing */
  CT_EQ(count_syncs(), quiet + 2);
  teardown();
}

static void test_rejoin_never_up_remote_pulses(void) {
  no_remote_up = 1;
  CT_EQ(setup_ex(0, 0), 0);
  no_remote_up = 0;
  settle(5);
  CT_EQ(chip_attached(), 0);
  CT_CHECK(count_syncs() >= 1); /* first awake event: the chip never reported, the link is still sought */
  teardown();
}

static void test_rejoin_failed_pulse_does_not_block_key(void) {
  setup();
  DRAIN();
  fr.fail_sync = 1;
  remote_chip_link(0); /* the immediate pulse write fails */
  int keys = nhkeys;
  fk_key(&fk, 0, 115, 1);
  tick(50);
  fk_key(&fk, 0, 115, 0);
  tick(50);
  WAIT_FOR(nhkeys == keys + 2); /* the press was not lost to a write backoff */
  teardown();
}

static void test_rejoin_followed_by_quick_uplink_in_5ms(void) {
  setup();
  DRAIN();
  int info = nhinfo;
  fh.drop_all = 1;
  remote_chip_link(0);
  int n = count_syncs();
  CT_EQ(n, 1);
  tick(RF_LINK_SYNC_QUERY_MS + 1);
  WAIT_FOR(nhinfo >= info || fr.nw > 0); /* settle */
  int w0 = 0;
  for (int i = 0; i < fr.nw && i < 512; i++) if (fr.w[i].len == 32 && fr.w[i].head[4] != HP_RF_KIND_POLL) w0++;
  CT_CHECK(w0 > 0);
  teardown();
}

static void test_rejoin_lost_mid_transfer_pulses_and_does_not_spin(void) {
  setup();
  DRAIN();
  fh.drop_all = 1;
  app.store.have_gen = true;
  app.store.gen = 1;
  app.store.total = 3;
  app.store.count = 1; /* a page set is arriving */
  remote_chip_link(0);
  CT_EQ(count_syncs(), 1); /* sent despite the transfer */
  app_link_t lk = rf_link_app(&rf);
  CT_CHECK(lk.next_ms(lk.ctx, fk.now) > 0); /* a future deadline, not a busy loop */
  idle_awake(40000);
  CT_EQ(count_syncs(), RF_LINK_SYNC_MAX_TRIES);
  CT_CHECK(lk.next_ms(lk.ctx, fk.now) > 0);
  app.store.have_gen = false;
  teardown();
}

static void test_rejoin_on_touch_while_lost(void) {
  setup();
  DRAIN();
  fh.drop_all = 1;
  remote_chip_link(0);
  idle_awake(40000);
  int base = count_syncs();
  CT_EQ(base, RF_LINK_SYNC_MAX_TRIES);
  fk_touch(&fk, 300, 300, 1);
  tick(50);
  fk_touch(&fk, 300, 300, 0);
  tick(50);
  CT_EQ(count_syncs(), base + 1);
  teardown();
}

int main(void) {
  CT_RUN(test_page_push_renders_golden);
  CT_RUN(test_theme_chain_renders_like_a_direct_render);
  CT_RUN(test_theme_chain_late_theme_completes_the_set);
  CT_RUN(test_theme_chain_corrupt_theme_is_ignored);
  CT_RUN(test_unknown_hub_message_type_is_acked_and_ignored);
  CT_RUN(test_keys_and_taps_reach_the_hub);
  CT_RUN(test_tile_update_changes_render);
  CT_RUN(test_tile_state_applies_and_unknown_types_are_dropped);
  CT_RUN(test_remote_info_is_first_uplink);
  CT_RUN(test_remote_info_reports_the_page_store);
  CT_RUN(test_remote_info_reports_a_store_it_already_holds);
  CT_RUN(test_dropped_ack_delivers_once);
  CT_RUN(test_peer_that_never_acks_shows_fallback_only);
  CT_RUN(test_polls_only_while_awake);
  CT_RUN(test_idle_link_is_silent_and_heartbeats);
  CT_RUN(test_rejoin_silent_while_link_up);
  CT_RUN(test_rejoin_on_loss_then_capped_backoff);
  CT_RUN(test_rejoin_on_keypress_while_lost);
  CT_RUN(test_rejoin_on_wake_and_resume);
  CT_RUN(test_rejoin_never_up_remote_pulses);
  CT_RUN(test_rejoin_failed_pulse_does_not_block_key);
  CT_RUN(test_rejoin_followed_by_quick_uplink_in_5ms);
  CT_RUN(test_rejoin_lost_mid_transfer_pulses_and_does_not_spin);
  CT_RUN(test_rejoin_on_touch_while_lost);
  CT_RUN(test_heartbeat_unreadable_soc);
  CT_RUN(test_wait_includes_link_fd_and_deadline);
  CT_RUN(test_open_refused_when_hal_alive);
  CT_RUN(test_cli);
  CT_RUN(test_shutdown_destroys_rfspi_once);
  CT_RUN(test_full_session_is_safe_with_rf);
  return CT_DONE();
}
