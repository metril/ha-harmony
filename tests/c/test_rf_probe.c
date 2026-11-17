/* Host-only tests for tools/rf_probe.c: every syscall is injected through rfp_ops_t, no device
 * node is touched. The tool is #included so its static functions are testable. */
#define RFP_TEST 1
#include "tools/rf_probe.c"

#include "ctest_assert.h"

enum { MAXLOG = 128, MAXRD = 64 };
typedef struct {
  int open_errno;
  struct { char op; char path[32]; uint8_t data[8]; int n; } log[MAXLOG]; /* o open, w write, r read, a alarm */
  int nlog;
  int wret[MAXLOG], nw, wi;     /* write() return script; exhausted = 0 (no frame back) */
  struct { uint8_t d[RFP_BUF]; int n; int overrun_to; } rd[MAXRD]; /* n<0: EINTR; overrun_to>n: scribble */
  int nrd, ri;
  int no_verfile;      /* 1: the version file is unreadable */
  const char *verfile; /* read_file() content; NULL = unreadable */
  int hal; /* what hal_alive() reports: 0 not alive (the default), 1 alive, -1 cannot tell */
} fake_t;

static void lg(fake_t *f, char op, const char *p, const void *d, int n) {
  if (f->nlog >= MAXLOG) return;
  f->log[f->nlog].op = op;
  if (p) snprintf(f->log[f->nlog].path, sizeof f->log[0].path, "%s", p);
  if (d) memcpy(f->log[f->nlog].data, d, n < 8 ? (size_t)n : 8);
  f->log[f->nlog++].n = n;
}
static int f_open(void *c, const char *p, int fl) {
  fake_t *f = c; (void)fl;
  lg(f, 'o', p, NULL, 0);
  if (f->open_errno) { errno = f->open_errno; return -1; }
  return 5;
}
static ssize_t f_write(void *c, int fd, const void *b, size_t n) {
  fake_t *f = c; (void)fd;
  lg(f, 'w', NULL, b, (int)n);
  if (f->wi >= f->nw) return 0;
  int r = f->wret[f->wi++];
  if (r < 0) errno = EIO;
  return r;
}
static ssize_t f_read(void *c, int fd, void *b, size_t cap) {
  fake_t *f = c; (void)fd;
  lg(f, 'r', NULL, NULL, (int)cap);
  if (f->ri >= f->nrd || f->rd[f->ri].n < 0) { if (f->ri < f->nrd) f->ri++; errno = EINTR; return -1; }
  int i = f->ri++, n = f->rd[i].n;
  memcpy(b, f->rd[i].d, (size_t)n);
  for (int k = n; k < f->rd[i].overrun_to && (size_t)k < cap; k++) ((uint8_t *)b)[k] = 0x55;
  return n;
}
static int f_close(void *c, int fd) { (void)c; (void)fd; return 0; }
static void f_alarm(void *c, unsigned s) { lg(c, 'a', NULL, NULL, (int)s); }
static int f_hal_alive(void *c) { return ((fake_t *)c)->hal; }
static ssize_t f_read_file(void *c, const char *p, void *b, size_t cap) {
  fake_t *f = c;
  lg(f, 'f', p, NULL, 0);
  if (!f->verfile) { errno = ENOENT; return -1; }
  size_t n = strlen(f->verfile);
  if (n > cap) n = cap;
  memcpy(b, f->verfile, n);
  return (ssize_t)n;
}
static const char *const STOCK_FILE = "39.00.0010\n"; /* the hub's file; the default for fakes */
static rfp_ops_t mk(fake_t *f) {
  if (!f->verfile && !f->no_verfile) f->verfile = STOCK_FILE;
  return (rfp_ops_t){f, f_open, f_write, f_read, f_close, f_alarm, f_hal_alive, f_read_file};
}

/* Like run_cmd but returns stderr, where the preflight refusal is written. */
static char *run_cmd_err(const rfp_ops_t *o, int argc, char **argv, int *rc) {
  char *buf = NULL; size_t sz = 0;
  char *ebuf = NULL; size_t esz = 0;
  FILE *out = open_memstream(&buf, &sz), *err = open_memstream(&ebuf, &esz);
  *rc = rfp_run(o, argc, argv, out, err);
  fclose(out);
  fclose(err);
  free(buf);
  return ebuf;
}

static void push_rd(fake_t *f, const uint8_t *d, int n) {
  f->rd[f->nrd].n = n;
  if (n > 0) memcpy(f->rd[f->nrd].d, d, (size_t)n);
  f->nrd++;
}
static void push_eintr(fake_t *f) { f->rd[f->nrd++].n = -1; }

static char *run_cmd(const rfp_ops_t *o, int argc, char **argv, int *rc) {
  char *buf = NULL; size_t sz = 0;
  char *ebuf = NULL; size_t esz = 0;
  FILE *out = open_memstream(&buf, &sz), *err = open_memstream(&ebuf, &esz);
  *rc = rfp_run(o, argc, argv, out, err);
  fclose(out);
  fclose(err);
  free(ebuf);
  return buf;
}

/* The cc2544 driver's open() is not exclusive, so it never returns EBUSY and an errno check cannot
 * detect stock hal. The preflight must refuse before opening, and must fail CLOSED when it cannot
 * tell -- opening the radio while hal holds it can hang the flash SPI bus the two share. */
static void test_preflight_refuses_when_hal_alive(void) {
  const char *subcmd[] = {"--probe", "--version", "--pipes", "--listen"};
  for (size_t i = 0; i < sizeof subcmd / sizeof subcmd[0]; i++) {
    for (int hal = 1; hal >= -1; hal -= 2) { /* 1 = alive, -1 = indeterminate */
      fake_t f = {0};
      f.hal = hal;
      rfp_ops_t o = mk(&f);
      char *argv[] = {"rf_probe", (char *)subcmd[i]};
      int rc;
      char *e = run_cmd_err(&o, 2, argv, &rc);
      CT_EQ(rc, 1);
      CT_CHECK(strstr(e, "refusing to open") && strstr(e, "quiesce"));
      CT_CHECK(strstr(e, hal > 0 ? "stock hal is alive" : "could not determine"));
      /* and crucially: the device was never opened */
      for (int k = 0; k < f.nlog; k++) CT_CHECK(f.log[k].op != 'o');
      free(e);
    }
  }
}

/* A NULL hal_alive in the ops table must also fail closed, not be treated as "no hal". */
static void test_preflight_fails_closed_without_probe_fn(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  o.hal_alive = NULL;
  char *argv[] = {"rf_probe", "--probe"};
  int rc;
  char *e = run_cmd_err(&o, 2, argv, &rc);
  CT_EQ(rc, 1);
  CT_CHECK(strstr(e, "could not determine"));
  for (int k = 0; k < f.nlog; k++) CT_CHECK(f.log[k].op != 'o');
  free(e);
}

static void test_frame_len(void) {
  static const struct { uint8_t b; size_t n; } t[] = {{0x00, 0}, {0x01, 7}, {0x02, 7}, {0x03, 4}, {0x04, 1}, {0x08, 1},
                                                      {0x10, 7}, {0x11, 20}, {0x12, 30}, {0x20, 32}};
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) CT_EQ(rfp_frame_len(t[i].b), t[i].n);
  for (int b = 0; b < 256; b++) { /* every other first byte is unknown -> 0 */
    int known = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) known |= t[i].b == b;
    if (!known) CT_EQ(rfp_frame_len((uint8_t)b), 0);
  }
}

static void test_write_ret_is_rx_len(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  rfp_xfer_t x;
  uint8_t rep[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 39, 0};
  f.wret[f.nw++] = 7; /* rx length 7 != "bytes written"; must not be treated as success/short-write */
  push_rd(&f, rep, 7);
  rfp_xfer(&o, 5, RFP_VER1, 0, 1, &x);
  CT_EQ(x.wret, 7); CT_EQ(x.status, 0); CT_EQ(x.rxlen, 7);
  /* wret 20 (long rx frame) for a 7-byte write is likewise fine, and 0 (no frame) is not an error */
  fake_t g = {0};
  o = mk(&g);
  g.wret[g.nw++] = 20;
  rfp_xfer(&o, 5, RFP_VER1, 0, 0, &x);
  CT_EQ(x.wret, 20); CT_EQ(x.status, 0);
  fake_t h = {0};
  o = mk(&h);
  rfp_xfer(&o, 5, RFP_VER1, 0, 0, &x);
  CT_EQ(x.wret, 0); CT_EQ(x.status, 0);
}

static void test_write_negative_is_io(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  rfp_xfer_t x;
  f.wret[f.nw++] = -1;
  rfp_xfer(&o, 5, RFP_VER1, 0, 1, &x);
  CT_EQ(x.status, -HP_E_IO);
  for (int i = 0; i < f.nlog; i++) CT_CHECK(f.log[i].op != 'r'); /* no read after a failed write */
}

static void test_reply_match(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  rfp_xfer_t x;
  uint8_t other[7] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 1, 2};  /* different 5th byte: skipped */
  uint8_t noise[3] = {1, 2, 3};                              /* too short: skipped */
  uint8_t good[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 39, 0};
  push_rd(&f, other, 7); push_rd(&f, noise, 3); push_rd(&f, good, 7);
  rfp_xfer(&o, 5, RFP_VER1, 0, 1, &x);
  CT_EQ(x.status, 0); CT_EQ(x.skipped, 2); CT_EQ(x.rxlen, 7); CT_MEM_EQ(x.rx, good, 7);
  /* alarm armed to 1 s before reading, cleared after */
  int armed = 0, cleared = 0;
  for (int i = 0; i < f.nlog; i++) if (f.log[i].op == 'a') { armed += f.log[i].n == 1; cleared += f.log[i].n == 0; }
  CT_EQ(armed, 1); CT_EQ(cleared, 1);
  /* every read buffer is >= 64 bytes */
  for (int i = 0; i < f.nlog; i++) if (f.log[i].op == 'r') CT_CHECK(f.log[i].n >= 64);
}

static void test_reply_timeout(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  rfp_xfer_t x;
  uint8_t other[7] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 1, 2};
  push_rd(&f, other, 7);
  push_eintr(&f);
  rfp_xfer(&o, 5, RFP_VER1, 0, 1, &x);
  CT_EQ(x.status, -HP_E_NOENT); CT_EQ(x.skipped, 1); CT_EQ(x.rxlen, 0);
}

static void test_pipe_query(void) {
  for (size_t i = 0; i < RFP_NPIPES; i++) {
    uint8_t c[7], want[7] = {0x10, 0xFF, 0x83, 0xB5, RFP_PIPES[i], 0, 0};
    rfp_pipe_cmd(RFP_PIPES[i], c);
    CT_MEM_EQ(c, want, 7);
    CT_CHECK(rfp_cmd_allowed(c, 7));
  }
  CT_EQ(RFP_NPIPES, 19);
  /* --pipes writes exactly those frames in order, matching 0x11-id replies */
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  for (size_t i = 0; i < RFP_NPIPES; i++) {
    uint8_t r[20] = {0x11, 0xFF, 0x83, 0xB5, RFP_PIPES[i]};
    push_rd(&f, r, 20);
  }
  char *argv[] = {"rf_probe", "--pipes"};
  int rc;
  char *txt = run_cmd(&o, 2, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "hub RF address") && strstr(txt, "EquadID") && strstr(txt, "remote name"));
  free(txt);
  int w = 0;
  for (int i = 0; i < f.nlog; i++) if (f.log[i].op == 'w') {
    uint8_t want[7] = {0x10, 0xFF, 0x83, 0xB5, RFP_PIPES[w], 0, 0};
    CT_MEM_EQ(f.log[i].data, want, 7);
    w++;
  }
  CT_EQ(w, RFP_NPIPES);
}

static void test_canary(void) {
  uint8_t b[RFP_BUF];
  memset(b, RFP_CANARY, sizeof b);
  memset(b, 1, 7);
  CT_EQ(rfp_canary_dirty(b, sizeof b, 7), 0);      /* wrote within len: clean */
  b[7] = 0xAA;
  CT_EQ(rfp_canary_dirty(b, sizeof b, 7), 0);      /* byte happens to equal canary: no false positive */
  b[20] = 0; b[63] = 1;
  CT_EQ(rfp_canary_dirty(b, sizeof b, 7), 2);      /* beyond len changed */
  CT_EQ(rfp_canary_dirty(b, sizeof b, 64), 0);     /* len == cap: nothing beyond */
  CT_EQ(rfp_canary_dirty(b, sizeof b, 100), 0);    /* len > cap clamps */
  /* via --listen: clean frame, then a frame the kernel scribbled past its length */
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  uint8_t fr[7] = {0x10, 0xFF, 0x81, 0xF1, 1, 2, 3};
  push_rd(&f, fr, 7);
  push_rd(&f, fr, 7); f.rd[1].overrun_to = 12; /* 5 bytes past len */
  push_eintr(&f);
  char *argv[] = {"rf_probe", "--listen", "3"};
  int rc;
  char *txt = run_cmd(&o, 3, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "bytes-beyond-len-changed=0"));
  CT_CHECK(strstr(txt, "bytes-beyond-len-changed=5"));
  CT_CHECK(strstr(txt, "overrun_frames=1"));
  CT_CHECK(strstr(txt, "report 0x10: 2"));
  free(txt);
}

/* read() ignores `count` and copies the entry's self-reported length, so a length past the 64-byte
 * entry buffer means the driver overran itself. RFP_BUF is twice RFP_RXMAX so that this is
 * observable rather than a stack overflow; it must be counted even with the canary intact. */
static void test_listen_len_beyond_entry_buffer(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  uint8_t fr[RFP_RXMAX + 8];
  memset(fr, 0x12, sizeof fr);
  push_rd(&f, fr, RFP_RXMAX + 8); /* 72 bytes: legal for our buffer, impossible for the driver */
  push_eintr(&f);
  char *argv[] = {"rf_probe", "--listen", "3"};
  int rc;
  char *txt = run_cmd(&o, 3, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "bytes-beyond-len-changed=0")); /* canary clean: everything was written */
  CT_CHECK(strstr(txt, "overrun_frames=1"));           /* still flagged, on length alone */
  free(txt);
  /* A length exactly at the entry-buffer size is normal and must not be flagged. */
  fake_t g = {0};
  rfp_ops_t o2 = mk(&g);
  push_rd(&g, fr, RFP_RXMAX);
  push_eintr(&g);
  char *txt2 = run_cmd(&o2, 3, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt2, "overrun_frames=0"));
  free(txt2);
}

static void test_open_and_cli(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  int rc;
  char *a0[] = {"rf_probe"};
  free(run_cmd(&o, 1, a0, &rc));
  CT_EQ(rc, 2); CT_EQ(f.nlog, 0); /* no args: usage, device never opened */
  f.open_errno = EBUSY;
  char *a1[] = {"rf_probe", "--probe"};
  char *txt = run_cmd(&o, 2, a1, &rc);
  CT_EQ(rc, 1); CT_CHECK(strstr(txt, "stock hal still has it")); free(txt);
  f.open_errno = ENOENT;
  txt = run_cmd(&o, 2, a1, &rc);
  CT_EQ(rc, 1); CT_CHECK(!strstr(txt, "stock hal")); free(txt);
  f.open_errno = 0; f.nlog = 0;
  char *a2[] = {"rf_probe", "--probe", "--json"};
  txt = run_cmd(&o, 3, a2, &rc);
  CT_EQ(rc, 0); CT_CHECK(strstr(txt, "\"free\":true")); free(txt);
  for (int i = 0; i < f.nlog; i++) CT_CHECK(f.log[i].op != 'w'); /* probe sends nothing */
}

static void test_version(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  uint8_t i0[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0, 0};
  uint8_t v1[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 39, 0};
  /* The real chip answered with these bytes on 2026-11-17. The build field is HEX -- stock parses
   * its version file with "%d.%d.%x" -- so 00 10 is build 0x0010 and matches the file's "0010".
   * The earlier expectation here was 00 0A printed in decimal, which only looked right because
   * 0x000A renders as "0010" in decimal; that fitted the assertion to a formatting bug. */
  uint8_t v2[7] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 0x00, 0x10};
  push_rd(&f, i0, 7); push_rd(&f, v1, 7); push_rd(&f, v2, 7);
  char *argv[] = {"rf_probe", "--version"};
  int rc;
  char *txt = run_cmd(&o, 2, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "decoded: 39.00.0010"));
  CT_CHECK(strstr(txt, "39.00.0010 (compare)"));
  CT_CHECK(!strstr(txt, "39.00.0016")); /* the decimal rendering must not come back */
  free(txt);
}

/* A build field whose hex and decimal renderings differ visibly, so the format is pinned. */
static void test_version_build_is_hex(void) {
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  uint8_t i0[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0, 0};
  uint8_t v1[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 39, 0};
  uint8_t v2[7] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 0x00, 0xAB};
  push_rd(&f, i0, 7); push_rd(&f, v1, 7); push_rd(&f, v2, 7);
  char *argv[] = {"rf_probe", "--version"};
  int rc;
  char *txt = run_cmd(&o, 2, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "decoded: 39.00.00AB")); /* not 00171 */
  free(txt);
}

/* Brick safety: whatever any subcommand does, the syscall log never shows /dev/rffw, a pairing
 * (B2), unpair or power-mode (first byte 0x20) frame; only allow-listed frames reach write(). */
static void check_log_safe(const fake_t *f) {
  for (int i = 0; i < f->nlog; i++) {
    if (f->log[i].op == 'o') CT_CHECK(!strcmp(f->log[i].path, "/dev/rfspi"));
    if (f->log[i].op == 'f') CT_CHECK(!strcmp(f->log[i].path, "/lib/firmware/cc2544.version"));
    if (f->log[i].op != 'w') continue;
    const uint8_t *d = f->log[i].data;
    CT_EQ(f->log[i].n, 7);
    CT_CHECK(rfp_cmd_allowed(d, 7));
    CT_CHECK(d[0] == 0x10 && d[2] != 0xB2 && d[0] != 0x20);
    CT_CHECK(d[2] == 0x80 || d[2] == 0x81 || d[2] == 0x83);
  }
}

static void test_brick_safety(void) {
  const char *modes[][3] = {{"--probe"}, {"--version"}, {"--pipes"}, {"--listen", "1"}};
  for (int m = 0; m < 4; m++) {
    fake_t f = {0};
    rfp_ops_t o = mk(&f);
    char *argv[] = {"rf_probe", (char *)modes[m][0], (char *)modes[m][1]};
    int rc;
    free(run_cmd(&o, modes[m][1] ? 3 : 2, argv, &rc));
    check_log_safe(&f);
  }
  /* the guard itself: dangerous frames are refused before write() */
  static const uint8_t bad[][7] = {{0x10, 0xFF, 0xB2, 0, 0, 0, 0},   /* pairing */
                                   {0x10, 0xFF, 0xB3, 0, 0, 0, 0},   /* unpair-ish */
                                   {0x20, 0x01, 0, 0, 0, 0, 0},      /* power mode */
                                   {0x10, 0xFF, 0x83, 0xB5, 0x50, 0, 0}, /* pipe outside the list */
                                   {0x10, 0xFF, 0x83, 0xB5, 0x03, 1, 0}};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    fake_t f = {0};
    rfp_ops_t o = mk(&f);
    rfp_xfer_t x;
    rfp_xfer(&o, 5, bad[i], 0, 1, &x);
    CT_EQ(x.status, -HP_E_INVAL); CT_EQ(f.nlog, 0);
  }
}

/* The remote's driver memcpys `count` into a 32-byte slab: nothing over 32 may reach write(). */
static void test_write_cap(void) {
  CT_EQ(RFP_WRITE_MAX, 32);
  CT_CHECK(RFP_CMD_LEN <= RFP_WRITE_MAX);
  fake_t f = {0};
  rfp_ops_t o = mk(&f);
  uint8_t buf[64] = {0};
  errno = 0;
  CT_EQ(rfp_write(&o, 5, buf, 33), -1);
  CT_EQ(errno, EMSGSIZE);
  CT_EQ(rfp_write(&o, 5, buf, 64), -1);
  CT_EQ(rfp_write(&o, 5, buf, (size_t)-1), -1);
  CT_EQ(f.nlog, 0); /* refused before the driver saw anything */
  CT_EQ(rfp_write(&o, 5, buf, 32), 0);
  CT_EQ(rfp_write(&o, 5, buf, 7), 0);
  CT_EQ(f.nlog, 2);
  for (int i = 0; i < f.nlog; i++) CT_CHECK(f.log[i].n <= 32);
  /* and every command any subcommand sends is within the cap */
  const char *modes[][3] = {{"--probe"}, {"--version"}, {"--pipes"}, {"--listen", "1"}};
  for (int m = 0; m < 4; m++) {
    fake_t g = {0};
    rfp_ops_t og = mk(&g);
    char *argv[] = {"rf_probe", (char *)modes[m][0], (char *)modes[m][1]};
    int rc;
    free(run_cmd(&og, modes[m][1] ? 3 : 2, argv, &rc));
    for (int i = 0; i < g.nlog; i++) if (g.log[i].op == 'w') CT_CHECK(g.log[i].n <= RFP_WRITE_MAX);
  }
}

/* The remote's chip is 72.00.0007: the stock version comes from the file, not a constant. */
static void test_stock_version_from_file(void) {
  uint8_t i0[7] = {0x10, 0xFF, 0x80, 0, 0, 0, 0}, v1[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 72, 0};
  uint8_t v2[7] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 0x00, 0x07};
  const char *files[] = {"72.00.0007\n", NULL, "bad\"quote\n", "\n"};
  const char *want[] = {"72.00.0007", "unavailable", "unavailable", "unavailable"};
  for (int i = 0; i < 4; i++) {
    for (int json = 0; json < 2; json++) {
      fake_t f = {0};
      f.verfile = files[i];
      f.no_verfile = files[i] == NULL;
      rfp_ops_t o = mk(&f);
      push_rd(&f, i0, 7); push_rd(&f, v1, 7); push_rd(&f, v2, 7);
      char *argv[] = {"rf_probe", "--version", "--json"};
      int rc;
      char *txt = run_cmd(&o, json ? 3 : 2, argv, &rc);
      CT_EQ(rc, 0);
      if (!json) CT_CHECK(strstr(txt, "decoded: 72.00.0007"));
      char pat[64];
      snprintf(pat, sizeof pat, json ? "\"stock_version_file\":\"%s\"" : "%s (compare)", want[i]);
      CT_CHECK(strstr(txt, pat));
      CT_CHECK(!strstr(txt, "39.00.0010")); /* no hard-coded hub version */
      free(txt);
      check_log_safe(&f);
    }
  }
}

int main(void) {
  CT_RUN(test_preflight_refuses_when_hal_alive);
  CT_RUN(test_preflight_fails_closed_without_probe_fn);
  CT_RUN(test_frame_len);
  CT_RUN(test_write_ret_is_rx_len);
  CT_RUN(test_write_negative_is_io);
  CT_RUN(test_reply_match);
  CT_RUN(test_reply_timeout);
  CT_RUN(test_pipe_query);
  CT_RUN(test_canary);
  CT_RUN(test_listen_len_beyond_entry_buffer);
  CT_RUN(test_open_and_cli);
  CT_RUN(test_version);
  CT_RUN(test_version_build_is_hex);
  CT_RUN(test_brick_safety);
  CT_RUN(test_write_cap);
  CT_RUN(test_stock_version_from_file);
  return CT_DONE();
}
