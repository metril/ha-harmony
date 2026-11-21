/* Host-only tests for tools/remote_probe.c: every syscall is injected through rpr_ops_t, no device
 * node is touched. The tool is #included so its static functions are testable. */
#define RPR_TEST 1
#include "tools/remote_probe.c"

#include "ctest_assert.h"

enum { MAXLOG = 256, MAXQ = 32 };
typedef struct {
  char op; /* o open, i ioctl, r read, c close, w wait */
  char path[40];
  int flags;
  unsigned long req;
} logent_t;

typedef struct {
  logent_t log[MAXLOG];
  int nlog;
  int missing[RPR_NEVENT]; /* event<i> open fails ENOENT */
  int fb_missing;
  int fd_node[16];         /* fd -> index into the path table, filled on open */
  const char *fd_path[16];
  int nextfd;
  /* canned answers */
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  struct input_absinfo abs[4];
  const char *names[RPR_NEVENT];
  /* keys: queued reads per fd, and a fake clock */
  struct { int fd; struct input_event ev[8]; int n; int err; } q[MAXQ];
  int nq, qi;
  uint64_t now;
} fake_t;

static void lg(fake_t *f, char op, const char *p, int flags, unsigned long req) {
  if (f->nlog >= MAXLOG) return;
  logent_t *l = &f->log[f->nlog++];
  l->op = op;
  l->flags = flags;
  l->req = req;
  if (p) snprintf(l->path, sizeof l->path, "%s", p);
}

static int f_open(void *c, const char *p, int fl) {
  fake_t *f = c;
  lg(f, 'o', p, fl, 0);
  if (!strcmp(p, RPR_FB) && f->fb_missing) { errno = ENOENT; return -1; }
  for (int i = 0; i < RPR_NEVENT; i++)
    if (!strcmp(p, RPR_EVENT[i]) && f->missing[i]) { errno = ENOENT; return -1; }
  int fd = 10 + f->nextfd++;
  f->fd_path[fd - 10] = p;
  return fd;
}
static int f_ioctl(void *c, int fd, unsigned long req, void *a) {
  fake_t *f = c;
  lg(f, 'i', f->fd_path[fd - 10], 0, req);
  if (req == FBIOGET_VSCREENINFO) { memcpy(a, &f->var, sizeof f->var); return 0; }
  if (req == FBIOGET_FSCREENINFO) { memcpy(a, &f->fix, sizeof f->fix); return 0; }
  if (_IOC_NR(req) == 0x06) {
    int idx = f->fd_path[fd - 10][strlen(f->fd_path[fd - 10]) - 1] - '0';
    if (!f->names[idx]) { errno = ENODEV; return -1; }
    snprintf(a, _IOC_SIZE(req), "%s", f->names[idx]);
    return 0;
  }
  for (size_t i = 0; i < RPR_NABS; i++)
    if (_IOC_NR(req) == 0x40u + RPR_ABS[i]) { memcpy(a, &f->abs[i], sizeof f->abs[i]); return 0; }
  errno = ENOTTY;
  return -1;
}
static ssize_t f_read(void *c, int fd, void *b, size_t n) {
  fake_t *f = c;
  lg(f, 'r', f->fd_path[fd - 10], 0, 0);
  for (int i = f->qi; i < f->nq; i++) {
    if (f->q[i].fd != fd || f->q[i].n < 0) continue;
    if (f->q[i].err) { f->q[i].n = -1; errno = f->q[i].err; return -1; }
    size_t bytes = (size_t)f->q[i].n * sizeof(struct input_event);
    CT_CHECK(bytes <= n);
    memcpy(b, f->q[i].ev, bytes);
    f->q[i].n = -1; /* consumed */
    return (ssize_t)bytes;
  }
  errno = EAGAIN;
  return -1;
}
static int f_close(void *c, int fd) { lg(c, 'c', ((fake_t *)c)->fd_path[fd - 10], 0, 0); return 0; }
static int f_wait(void *c, const int *fds, int n, unsigned ms, int *ready) {
  fake_t *f = c;
  lg(f, 'w', NULL, 0, 0);
  int cnt = 0;
  for (int k = 0; k < n; k++) {
    ready[k] = 0;
    for (int i = 0; i < f->nq; i++)
      if (f->q[i].fd == fds[k] && f->q[i].n >= 0) { ready[k] = 1; break; }
    cnt += ready[k];
  }
  if (!cnt) f->now += ms; /* nothing pending: the whole timeout elapses */
  else f->now += 10;
  return cnt;
}
static uint64_t f_now(void *c) { return ((fake_t *)c)->now; }
static rpr_ops_t mk(fake_t *f) { return (rpr_ops_t){f, f_open, f_ioctl, f_read, f_close, f_wait, f_now}; }

static char *run_cmd(const rpr_ops_t *o, int argc, char **argv, int *rc) {
  char *buf = NULL, *ebuf = NULL;
  size_t sz = 0, esz = 0;
  FILE *out = open_memstream(&buf, &sz), *err = open_memstream(&ebuf, &esz);
  *rc = rpr_run(o, argc, argv, out, err);
  fclose(out);
  fclose(err);
  free(ebuf);
  return buf;
}

static void set_event(struct input_event *e, unsigned type, unsigned code, int value) {
  memset(e, 0, sizeof *e);
  e->type = (uint16_t)type;
  e->code = (uint16_t)code;
  e->value = value;
}

static fake_t *new_fake(void) {
  fake_t *f = calloc(1, sizeof *f);
  f->var.xres = 240; f->var.yres = 320; f->var.xres_virtual = 240; f->var.yres_virtual = 960;
  f->var.bits_per_pixel = 16;
  f->var.red = (struct fb_bitfield){11, 5, 0};
  f->var.green = (struct fb_bitfield){5, 6, 0};
  f->var.blue = (struct fb_bitfield){0, 5, 0};
  f->fix.line_length = 480; f->fix.smem_len = 460800;
  f->abs[0] = (struct input_absinfo){0, 0, 4095, 4, 8, 3};
  f->abs[1] = (struct input_absinfo){0, 0, 4095, 5, 9, 2};
  f->abs[2] = (struct input_absinfo){0, 0, 255, 1, 0, 0};
  f->abs[3] = (struct input_absinfo){0, 0, 15, 0, 0, 0};
  f->names[0] = "keypad"; f->names[2] = "touch \"panel\""; f->names[3] = "gauge";
  return f;
}

/* Brick safety, asserted over the syscall log of every subcommand: only /dev/fb0 and
 * /dev/input/event0..3, all O_RDONLY, and only the read-only ioctl set. */
static void check_log_safe(const fake_t *f) {
  for (int i = 0; i < f->nlog; i++) {
    const logent_t *l = &f->log[i];
    if (l->op == 'o') {
      int ok = !strcmp(l->path, "/dev/fb0");
      for (int k = 0; k < RPR_NEVENT; k++) ok |= !strcmp(l->path, RPR_EVENT[k]);
      CT_CHECK(ok);
      CT_EQ(l->flags, O_RDONLY);
      CT_CHECK(!strstr(l->path, "rffw") && !strstr(l->path, "mspfw") && !strstr(l->path, "watchdog") &&
               !strstr(l->path, "mtd") && !strstr(l->path, "rfspi") && !strstr(l->path, "/sys"));
    }
    if (l->op == 'i') {
      unsigned long r = l->req;
      CT_CHECK(r == FBIOGET_VSCREENINFO || r == FBIOGET_FSCREENINFO || _IOC_NR(r) == 0x06 ||
               (_IOC_TYPE(r) == 'E' && _IOC_NR(r) >= 0x40 && _IOC_NR(r) < 0x40 + ABS_MAX));
      CT_CHECK(r != EVIOCGRAB && r != FBIOPUT_VSCREENINFO && r != FBIOBLANK && r != FBIOPAN_DISPLAY);
      CT_EQ(_IOC_DIR(r) & _IOC_WRITE, 0); /* no kernel-bound data */
    }
  }
}

static void test_usage(void) {
  fake_t *f = new_fake();
  rpr_ops_t o = mk(f);
  int rc;
  char *a0[] = {"remote_probe"};
  free(run_cmd(&o, 1, a0, &rc));
  CT_EQ(rc, 2); CT_EQ(f->nlog, 0); /* no args: usage, nothing opened */
  const char *bad[][3] = {{"bogus"}, {"keys"}, {"keys", "0"}, {"keys", "601"}, {"keys", "x"}, {"keys", ""}, {"--json"}};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    int n = bad[i][1] ? 3 : 2;
    char *argv[] = {"remote_probe", (char *)bad[i][0], (char *)bad[i][1]};
    free(run_cmd(&o, bad[i][0][0] == '-' ? 2 : n, argv, &rc));
    CT_EQ(rc, 2);
  }
  CT_EQ(f->nlog, 0);
  free(f);
}

static void test_fbinfo(void) {
  for (int json = 0; json < 2; json++) {
    fake_t *f = new_fake();
    rpr_ops_t o = mk(f);
    char *argv[] = {"remote_probe", "fbinfo", "--json"};
    int rc;
    char *t = run_cmd(&o, json ? 3 : 2, argv, &rc);
    CT_EQ(rc, 0);
    if (json) {
      CT_CHECK(strstr(t, "\"xres\":240,\"yres\":320,\"xres_virtual\":240,\"yres_virtual\":960"));
      CT_CHECK(strstr(t, "\"red\":{\"offset\":11,\"length\":5,\"msb_right\":0}"));
      CT_CHECK(strstr(t, "\"green\":{\"offset\":5,\"length\":6"));
      CT_CHECK(strstr(t, "\"line_length\":480,\"smem_len\":460800"));
    } else {
      CT_CHECK(strstr(t, "xres=240 yres=320 virtual=240x960 bpp=16"));
      CT_CHECK(strstr(t, "red    offset=11 length=5"));
      CT_CHECK(strstr(t, "blue   offset=0 length=5"));
      CT_CHECK(strstr(t, "line_length=480 smem_len=460800"));
    }
    free(t);
    CT_EQ(f->nlog, 4); /* open, 2 ioctls, close: no mmap/write seam exists at all */
    check_log_safe(f);
    free(f);
  }
  fake_t *g = new_fake();
  g->fb_missing = 1;
  rpr_ops_t o = mk(g);
  char *argv[] = {"remote_probe", "fbinfo"};
  int rc;
  free(run_cmd(&o, 2, argv, &rc));
  CT_EQ(rc, 1);
  check_log_safe(g);
  free(g);
}

static void test_absinfo(void) {
  fake_t *f = new_fake();
  rpr_ops_t o = mk(f);
  int rc;
  char *argv[] = {"remote_probe", "absinfo"};
  char *t = run_cmd(&o, 2, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(t, "ABS_X: min=0 max=4095 fuzz=4 flat=8 resolution=3"));
  CT_CHECK(strstr(t, "ABS_Y: min=0 max=4095 fuzz=5 flat=9 resolution=2"));
  CT_CHECK(strstr(t, "ABS_PRESSURE: min=0 max=255"));
  CT_CHECK(strstr(t, "ABS_TOOL_WIDTH: min=0 max=15"));
  free(t);
  char *aj[] = {"remote_probe", "--json", "absinfo"};
  t = run_cmd(&o, 3, aj, &rc);
  CT_CHECK(strstr(t, "\"name\":\"ABS_X\",\"code\":0,\"ok\":true,\"min\":0,\"max\":4095,\"fuzz\":4,\"flat\":8,\"resolution\":3"));
  free(t);
  int opens = 0;
  for (int i = 0; i < f->nlog; i++)
    if (f->log[i].op == 'o') { CT_CHECK(!strcmp(f->log[i].path, "/dev/input/event2")); opens++; }
  CT_EQ(opens, 2);
  check_log_safe(f);
  free(f);
}

static void test_name(void) {
  fake_t *f = new_fake();
  f->missing[1] = 1; /* event1 absent: reported, not fatal */
  rpr_ops_t o = mk(f);
  int rc;
  char *argv[] = {"remote_probe", "name"};
  char *t = run_cmd(&o, 2, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(t, "/dev/input/event0: \"keypad\""));
  CT_CHECK(strstr(t, "/dev/input/event1: unavailable"));
  CT_CHECK(strstr(t, "/dev/input/event2: \"touch \"panel\"\""));
  CT_CHECK(strstr(t, "/dev/input/event3: \"gauge\""));
  free(t);
  char *aj[] = {"remote_probe", "name", "--json"};
  t = run_cmd(&o, 3, aj, &rc);
  CT_CHECK(strstr(t, "\"name\":\"touch \\\"panel\\\"\"")); /* JSON-escaped */
  CT_CHECK(strstr(t, "\"path\":\"/dev/input/event1\",\"ok\":false"));
  free(t);
  check_log_safe(f);
  free(f);
  fake_t *g = new_fake();
  for (int i = 0; i < RPR_NEVENT; i++) g->missing[i] = 1;
  o = mk(g);
  free(run_cmd(&o, 2, argv, &rc));
  CT_EQ(rc, 1); /* nothing readable at all */
  free(g);
}

/* Real struct input_event layout (native: 16 bytes on the 32-bit remote, 24 on a 64-bit host). */
static void test_keys(void) {
  fake_t *f = new_fake();
  rpr_ops_t o = mk(f);
  struct input_event ev[5];
  set_event(&ev[0], EV_KEY, 115, 1);  /* VolumeUp press */
  set_event(&ev[1], EV_SYN, 0, 0);    /* ignored */
  set_event(&ev[2], EV_KEY, 115, 0);  /* release */
  set_event(&ev[3], EV_KEY, 9999, 1); /* unknown code */
  set_event(&ev[4], EV_REL, 1, 5);    /* ignored */
  f->q[f->nq].fd = 10; memcpy(f->q[f->nq].ev, ev, sizeof ev); f->q[f->nq++].n = 5;
  struct input_event t[2];
  set_event(&t[0], EV_ABS, ABS_X, 1234);
  set_event(&t[1], EV_ABS, ABS_PRESSURE, 77);
  f->q[f->nq].fd = 11; memcpy(f->q[f->nq].ev, t, sizeof t); f->q[f->nq++].n = 2;
  int rc;
  char *argv[] = {"remote_probe", "keys", "5"};
  char *txt = run_cmd(&o, 3, argv, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "event0 EV_KEY code=115 (VolumeUp) value=1"));
  CT_CHECK(strstr(txt, "event0 EV_KEY code=115 (VolumeUp) value=0"));
  CT_CHECK(strstr(txt, "event0 EV_KEY code=9999 (unknown) value=1"));
  CT_CHECK(strstr(txt, "event2 EV_ABS code=0 (ABS_X) value=1234"));
  CT_CHECK(strstr(txt, "event2 EV_ABS code=24 (ABS_PRESSURE) value=77"));
  CT_CHECK(!strstr(txt, "EV_SYN") && !strstr(txt, "EV_REL"));
  CT_CHECK(strstr(txt, "events printed: 5"));
  free(txt);
  CT_EQ(f->now, 5000); /* ran the full 5 s of the fake clock */
  check_log_safe(f);
  free(f);

  /* JSON, a node missing (event2): keeps logging event0 */
  fake_t *g = new_fake();
  g->missing[2] = 1;
  o = mk(g);
  g->q[g->nq].fd = 10; memcpy(g->q[g->nq].ev, ev, sizeof ev); g->q[g->nq++].n = 1;
  char *aj[] = {"remote_probe", "keys", "1", "--json"};
  txt = run_cmd(&o, 4, aj, &rc);
  CT_EQ(rc, 0);
  CT_CHECK(strstr(txt, "{\"dev\":\"event0\",\"type\":\"EV_KEY\",\"code\":115,\"name\":\"VolumeUp\",\"value\":1}"));
  CT_CHECK(strstr(txt, "\"total\":1}"));
  free(txt);
  check_log_safe(g);
  free(g);

  /* both nodes missing: failure, nothing waited on */
  fake_t *h = new_fake();
  h->missing[0] = h->missing[2] = 1;
  o = mk(h);
  free(run_cmd(&o, 3, argv, &rc));
  CT_EQ(rc, 1);
  for (int i = 0; i < h->nlog; i++) CT_CHECK(h->log[i].op != 'w');
  free(h);
}

static void test_keys_all_stock_names(void) {
  static const struct { unsigned c; const char *n; } want[] = {
      {103, "Up"}, {108, "Down"}, {105, "Left"}, {106, "Right"}, {352, "Select"}, {139, "Menu"}, {174, "Exit"},
      {115, "VolumeUp"}, {114, "VolumeDown"}, {113, "VolumeMute"}, {402, "ChannelUp"}, {403, "ChannelDown"},
      {412, "PrevChannel"}, {207, "Play"}, {119, "Pause"}, {128, "Stop"}, {167, "Record"}, {168, "Rewind"},
      {159, "FastForward"}, {389, "DVR"}, {395, "Guide"}, {358, "Info"}, {398, "Red"}, {399, "Green"},
      {400, "Yellow"}, {401, "Blue"}, {148, "HomeControl1"}, {202, "HomeControl2"}, {149, "HomeControl3"},
      {203, "HomeControl4"}, {78, "HomeControlPlus"}, {74, "HomeControlMinus"}, {116, "PowerOffActivity"},
      {364, "Home"}, {102, "Favorites"}};
  CT_EQ(sizeof want / sizeof want[0], 35);
  CT_EQ(sizeof RPR_KEYS / sizeof RPR_KEYS[0], 35);
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) CT_CHECK(!strcmp(rpr_key_name(want[i].c), want[i].n));
  CT_CHECK(!strcmp(rpr_key_name(0), "unknown"));
}

/* A node that disappears mid-run (ENODEV) is dropped; the other keeps being read. */
static void test_keys_read_error(void) {
  fake_t *f = new_fake();
  rpr_ops_t o = mk(f);
  f->q[f->nq].fd = 10; f->q[f->nq++].err = ENODEV;
  struct input_event e;
  set_event(&e, EV_KEY, 364, 1);
  f->q[f->nq].fd = 11; f->q[f->nq].ev[0] = e; f->q[f->nq++].n = 1;
  int rc;
  char *argv[] = {"remote_probe", "keys", "2"};
  char *t = run_cmd(&o, 3, argv, &rc);
  CT_EQ(rc, 1);
  CT_CHECK(strstr(t, "event2 EV_KEY code=364 (Home) value=1"));
  free(t);
  check_log_safe(f);
  free(f);
}

/* The guard itself: anything off the allow-list is refused before the seam is reached. */
static void test_guard_refuses(void) {
  fake_t *f = new_fake();
  rpr_ops_t o = mk(f);
  static const char *const bad[] = {"/dev/rffw", "/dev/mspfw", "/dev/watchdog", "/dev/mtd0", "/dev/mtdblock7",
                                    "/dev/rfspi", "/sys/class/leds/x/brightness", "/dev/input/event4",
                                    "/dev/input/event", "/dev/fb1", "/dev/input/event0/", "/dev/fb0x", ""};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    errno = 0;
    CT_EQ(rpr_open(&o, bad[i], O_RDONLY), -1);
    CT_EQ(errno, EPERM);
  }
  CT_EQ(rpr_open(&o, "/dev/fb0", O_RDWR), -1);          /* right path, writable */
  CT_EQ(rpr_open(&o, "/dev/fb0", O_WRONLY), -1);
  CT_EQ(rpr_open(&o, "/dev/input/event0", O_RDONLY | O_NONBLOCK), -1);
  CT_EQ(f->nlog, 0);
  int n = 0;
  CT_CHECK(rpr_open(&o, "/dev/input/event3", O_RDONLY) >= 0);
  unsigned long badio[] = {EVIOCGRAB, FBIOPUT_VSCREENINFO, FBIOBLANK, FBIOPAN_DISPLAY, FBIOGETCMAP,
                           EVIOCGABS(ABS_MT_SLOT), EVIOCGABS(ABS_Y + 1), EVIOCGBIT(0, 8), EVIOCGKEY(8),
                           EVIOCSABS(ABS_X), EVIOCGVERSION, 0};
  for (size_t i = 0; i < sizeof badio / sizeof badio[0]; i++) {
    errno = 0;
    CT_EQ(rpr_ioctl(&o, 10, badio[i], &n), -1);
    CT_EQ(errno, EPERM);
  }
  CT_EQ(f->nlog, 1); /* just the one allowed open */
  check_log_safe(f);
  free(f);
}

int main(void) {
  CT_RUN(test_usage);
  CT_RUN(test_fbinfo);
  CT_RUN(test_absinfo);
  CT_RUN(test_name);
  CT_RUN(test_keys);
  CT_RUN(test_keys_all_stock_names);
  CT_RUN(test_keys_read_error);
  CT_RUN(test_guard_refuses);
  return CT_DONE();
}
