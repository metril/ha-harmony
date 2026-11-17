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
#include "remote/app/rf_link.h"
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
    if (f->is_remote && n == 32 && p[0] == 0x20) { /* the remote is slot 1 on the hub */
      o[0] = 0x12, o[1] = 1;
      memcpy(o + 2, p + 4, 28);
      feed(peer, o, 30);
    } else if (!f->is_remote && n == 30 && p[0] == 0x12) { /* the hub is slot 0 on the remote */
      o[0] = 0x20, o[1] = 0, o[2] = 0x12, o[3] = 0;
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
typedef struct { uint8_t remote, pct; uint16_t mv; uint64_t at; } hbatt_t;

static fk_t fk;
static app_t app;
static rf_link_t rf;
static fake_t fh, fr; /* hub chip, remote chip */
static hp_rf_transport *hub_tp;
static hp_rf_link_t hub_link;
static hkey_t hkeys[256];
static int nhkeys;
static hbatt_t hbatt[32];
static int nhbatt, nhother;

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
    if (nhbatt < 32) hbatt[nhbatt++] = (hbatt_t){(uint8_t)v[0].u, (uint8_t)v[1].u, (uint16_t)v[2].u, fk.now};
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

static int setup_ex(int hal, int soc_missing) {
  fk_init(&fk);
  if (soc_missing) fk_set_file(&fk, RA_PATH_SOC, NULL);
  fake_init(&fh, 0);
  fake_init(&fr, 1);
  fh.peer = &fr;
  fr.peer = &fh;
  fr.hal = hal;
  nhkeys = nhbatt = nhother = 0;
  rec_n = 0;
  hp_rf_spi_ops_t ho = fake_ops(&fh);
  hp_rf_spi_cfg_t hc = {NULL, 0, &ho, HP_RF_SPI_ROLE_HUB, 0};
  CT_EQ(hp_rf_spi_open(&hc, &hub_tp), 0);
  CT_EQ(hub_tp->claim(hub_tp->self), 0);
  hp_rf_link_init(&hub_link, hub_tp, false, 1, hub_deliver, NULL);

  ra_ops_t ops = fk_ops(&fk);
  ops.wait = rec_wait;
  rf_link_init(&rf, &ops, RF_LINK_REMOTE_ID);
  app_link_t link = rf_link_app(&rf);
  CT_EQ(app_init(&app, &ops, &link, NULL), 0);
  hp_rf_spi_ops_t ro = fake_ops(&fr);
  hp_rf_spi_cfg_t rc = {NULL, 0, &ro, HP_RF_SPI_ROLE_REMOTE, 0};
  return rf_link_open(&rf, &rc);
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
  snprintf(got, sizeof got, "%016llx", (unsigned long long)fb_hash());
  CT_CHECK(!strcmp(got, hash));
  if (strcmp(got, hash)) fprintf(stderr, "render %s golden %s\n", got, hash);
  CT_CHECK(!app.ui.fallback);
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
  fk_touch(&fk, 10, 10, 1);
  tick(50);
  fk_touch(&fk, 10, 10, 0);
  tick(50);
  WAIT_FOR(nhkeys == base + 4);
  CT_EQ(hkeys[base + 2].key, HP_SOFT_KEY_BASE + 1);
  CT_EQ(hkeys[base + 2].action, HP_KEY_ACTION_PRESS);
  CT_EQ(hkeys[base + 3].key, HP_SOFT_KEY_BASE + 1);
  CT_EQ(hkeys[base + 3].action, HP_KEY_ACTION_RELEASE);
  CT_CHECK(!app.ui.fallback);
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
  CT_EQ(hbatt[1].pct, 42);
  CT_EQ(hbatt[1].at - t0, RF_LINK_BATTERY_MS);
  CT_EQ(hbatt[2].at - t0, 2 * RF_LINK_BATTERY_MS);
  CT_EQ(nhother, 0);
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
  CT_CHECK(rec_ms >= 1000 && rec_ms <= APP_LINK_IDLE_WAIT_MS); /* idle: rare wakeups, still capped */
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
      {"--slot", "16", NULL},       {"--slot", "-1", NULL},     {"--slot", "", NULL},       {"--slot", "1x", NULL},
      {"--slot", NULL, NULL},       {"--bogus", "1", NULL},     {"rf", "spi", NULL},
  };
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *av[4] = {"x", (char *)bad[i][0], (char *)bad[i][1], NULL};
    CT_EQ(ra_cli_parse(bad[i][1] ? 3 : 2, av, &c), -1);
  }
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
  fk_touch(&fk, 10, 10, 1);
  tick(100);
  fk_touch(&fk, 10, 10, 0);
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

int main(void) {
  CT_RUN(test_page_push_renders_golden);
  CT_RUN(test_keys_and_taps_reach_the_hub);
  CT_RUN(test_tile_update_changes_render);
  CT_RUN(test_dropped_ack_delivers_once);
  CT_RUN(test_peer_that_never_acks_shows_fallback_only);
  CT_RUN(test_idle_link_is_silent_and_heartbeats);
  CT_RUN(test_heartbeat_unreadable_soc);
  CT_RUN(test_wait_includes_link_fd_and_deadline);
  CT_RUN(test_open_refused_when_hal_alive);
  CT_RUN(test_cli);
  CT_RUN(test_shutdown_destroys_rfspi_once);
  CT_RUN(test_full_session_is_safe_with_rf);
  return CT_DONE();
}
