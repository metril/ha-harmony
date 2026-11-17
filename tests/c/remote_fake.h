#ifndef REMOTE_FAKE_H
#define REMOTE_FAKE_H

/* Host fake for the remote app's syscall seam (ra_ops_t) and link (app_link_t): an in-memory
 * 240x320 RGB565 framebuffer, two scripted evdev nodes, sysfs files, a fake clock, and a log of
 * every syscall so tests can assert the brick-safety allow-list over a whole session. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctest_assert.h"
#include "proto/gen/harmony_proto.h"
#include "remote/app/app.h"

#define FK_MAXLOG 4096
#define FK_FB_BYTES (240 * 320 * 2)

typedef struct {
  char path[96];
  char data[64];
  int exists;
} fk_file_t;

typedef struct {
  char log[FK_MAXLOG][112]; /* "open <path> <flags>", "ioctl <req>", "mmap", "exec <path>", ... */
  int nlog;
  struct { int used, dev; char path[96]; } fds[32]; /* dev: 0/1 for event0/event2, -1 otherwise */
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  uint8_t fb[FK_FB_BYTES];
  struct input_event q[2][512];
  int qn[2], qh[2];
  fk_file_t files[8];
  int nfiles;
  int bl_w[2048], nbl, led_w[64], nled;
  int boot_writes;
  char bootcount[8];
  int nexec;
  int nblank, blank_arg; /* FBIOBLANK calls seen and the last level */
  uint64_t now;
  char msgs[64][160];
  int nmsgs;
} fk_t;

static __attribute__((unused)) void fk_log(fk_t *f, const char *fmt, ...) {
  if (f->nlog >= FK_MAXLOG) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(f->log[f->nlog++], sizeof f->log[0], fmt, ap);
  va_end(ap);
}

static __attribute__((unused)) fk_file_t *fk_file(fk_t *f, const char *path) {
  for (int i = 0; i < f->nfiles; i++)
    if (!strcmp(f->files[i].path, path)) return &f->files[i];
  return NULL;
}

static __attribute__((unused)) void fk_set_file(fk_t *f, const char *path, const char *data) {
  fk_file_t *x = fk_file(f, path);
  if (!x) {
    x = &f->files[f->nfiles++];
    snprintf(x->path, sizeof x->path, "%s", path);
  }
  snprintf(x->data, sizeof x->data, "%s", data ? data : "");
  x->exists = data != NULL;
}

static __attribute__((unused)) int fk_open(void *c, const char *p, int fl, int mode) {
  fk_t *f = c;
  (void)mode;
  fk_log(f, "open %s %d", p, fl);
  int dev = -2;
  if (!strcmp(p, "/dev/fb0")) dev = -1;
  else if (!strcmp(p, "/dev/input/event0")) dev = 0;
  else if (!strcmp(p, "/dev/input/event2")) dev = 1;
  else {
    fk_file_t *x = fk_file(f, p);
    if (!x || (!x->exists && !(fl & O_CREAT))) { errno = ENOENT; return -1; }
    dev = -1;
  }
  for (int i = 3; i < 32; i++)
    if (!f->fds[i].used) {
      f->fds[i].used = 1;
      f->fds[i].dev = dev;
      snprintf(f->fds[i].path, sizeof f->fds[i].path, "%s", p);
      return i;
    }
  errno = EMFILE;
  return -1;
}

static __attribute__((unused)) int fk_close(void *c, int fd) {
  fk_t *f = c;
  fk_log(f, "close");
  f->fds[fd].used = 0;
  return 0;
}

static __attribute__((unused)) ssize_t fk_read(void *c, int fd, void *buf, size_t n) {
  fk_t *f = c;
  if (!f->fds[fd].used) { errno = EBADF; return -1; }
  int d = f->fds[fd].dev;
  if (d >= 0) {
    size_t k = 0;
    while (f->qh[d] < f->qn[d] && (k + 1) * sizeof(struct input_event) <= n) {
      ((struct input_event *)buf)[k++] = f->q[d][f->qh[d]++];
    }
    return (ssize_t)(k * sizeof(struct input_event));
  }
  fk_file_t *x = fk_file(f, f->fds[fd].path);
  if (!x) { errno = EIO; return -1; }
  size_t l = strlen(x->data);
  if (l > n) l = n;
  memcpy(buf, x->data, l);
  return (ssize_t)l;
}

static __attribute__((unused)) ssize_t fk_write(void *c, int fd, const void *buf, size_t n) {
  fk_t *f = c;
  const char *p = f->fds[fd].path;
  char s[16] = {0};
  memcpy(s, buf, n < 15 ? n : 15);
  if (!strcmp(p, "/sys/class/backlight/ap3156_backlight/brightness") && f->nbl < 2048) f->bl_w[f->nbl++] = atoi(s);
  else if (!strcmp(p, "/sys/class/leds/tps6507x:white/brightness") && f->nled < 64) f->led_w[f->nled++] = atoi(s);
  else {
    f->boot_writes++;
    snprintf(f->bootcount, sizeof f->bootcount, "%.*s", (int)(n < 7 ? n : 7), (const char *)buf);
  }
  return (ssize_t)n;
}

static __attribute__((unused)) int fk_ioctl(void *c, int fd, unsigned long req, void *arg) {
  fk_t *f = c;
  (void)fd;
  fk_log(f, "ioctl %#lx", req);
  if (req == FBIOGET_VSCREENINFO) memcpy(arg, &f->var, sizeof f->var);
  else if (req == FBIOGET_FSCREENINFO) memcpy(arg, &f->fix, sizeof f->fix);
  else if (req == FBIOBLANK) {
    f->nblank++;
    f->blank_arg = (int)(intptr_t)arg;
  } else { errno = ENOTTY; return -1; }
  return 0;
}

static __attribute__((unused)) void *fk_mmap(void *c, int fd, size_t len) {
  fk_t *f = c;
  (void)fd;
  fk_log(f, "mmap %zu", len);
  return len <= FK_FB_BYTES ? f->fb : NULL;
}

static __attribute__((unused)) int fk_munmap(void *c, void *p, size_t len) { (void)c; (void)p; (void)len; return 0; }

static __attribute__((unused)) int fk_wait(void *c, const int *fds, int n, unsigned ms, int *ready) {
  fk_t *f = c;
  (void)ms;
  int r = 0;
  for (int i = 0; i < n; i++) {
    int d = fds[i] >= 0 && fds[i] < 32 ? f->fds[fds[i]].dev : -1; /* fds beyond the table: the link's */
    ready[i] = d >= 0 && f->qh[d] < f->qn[d];
    r += ready[i];
  }
  return r;
}

static __attribute__((unused)) uint64_t fk_now(void *c) { return ((fk_t *)c)->now; }

static __attribute__((unused)) int fk_exec(void *c, const char *path) {
  fk_t *f = c;
  fk_log(f, "exec %s", path);
  f->nexec++;
  return 0;
}

static __attribute__((unused)) void fk_logmsg(void *c, const char *m) {
  fk_t *f = c;
  if (f->nmsgs < 64) snprintf(f->msgs[f->nmsgs++], sizeof f->msgs[0], "%s", m);
}

static __attribute__((unused)) ra_ops_t fk_ops(fk_t *f) {
  return (ra_ops_t){.ctx = f, .open = fk_open, .close = fk_close, .read = fk_read, .write = fk_write,
                    .ioctl = fk_ioctl, .mmap = fk_mmap, .munmap = fk_munmap, .wait = fk_wait,
                    .now_ms = fk_now, .exec_poweroff = fk_exec, .log = fk_logmsg};
}

/* The Elite as measured: 240x320 RGB565, line_length 480, a healthy battery on the cradle. */
static __attribute__((unused)) void fk_init(fk_t *f) {
  memset(f, 0, sizeof *f);
  f->var.xres = 240;
  f->var.yres = 320;
  f->var.xres_virtual = 240;
  f->var.yres_virtual = 960;
  f->var.bits_per_pixel = 16;
  f->var.red = (struct fb_bitfield){11, 5, 0};
  f->var.green = (struct fb_bitfield){5, 6, 0};
  f->var.blue = (struct fb_bitfield){0, 5, 0};
  f->fix.line_length = 480;
  f->fix.smem_len = 466944;
  f->now = 1000;
  fk_set_file(f, "/sys/class/backlight/ap3156_backlight/brightness", "26");
  fk_set_file(f, "/sys/class/leds/tps6507x:white/brightness", "0");
  fk_set_file(f, "/sys/bus/i2c/devices/1-0070/soc", "100.0\n");
  fk_set_file(f, "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/powersource", "cradle\n");
  fk_set_file(f, "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/battcharge", "0.00 0x0000\n");
  fk_set_file(f, "/tmp/bootcount", NULL); /* absent until the app writes it */
}

static __attribute__((unused)) void fk_push(fk_t *f, int dev, int type, int code, int value) {
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = (uint16_t)type;
  e.code = (uint16_t)code;
  e.value = value;
  if (f->qn[dev] < 512) f->q[dev][f->qn[dev]++] = e;
}
static __attribute__((unused)) void fk_key(fk_t *f, int dev, int code, int value) {
  fk_push(f, dev, EV_KEY, code, value);
  fk_push(f, dev, EV_SYN, SYN_REPORT, 0);
}
static __attribute__((unused)) void fk_touch(fk_t *f, int x, int y, int down) {
  fk_push(f, 1, EV_ABS, ABS_X, x);
  fk_push(f, 1, EV_ABS, ABS_Y, y);
  fk_push(f, 1, EV_KEY, BTN_TOUCH, down);
  fk_push(f, 1, EV_SYN, SYN_REPORT, 0);
}

/* Link fake: records send_key, replays queued events. */
typedef struct {
  struct { uint8_t code, action; } sent[512];
  int nsent;
  app_link_event_t q[64];
  int nq, qh;
  int awake, nawake; /* last set_awake value and number of calls */
} fk_link_t;

static __attribute__((unused)) int fkl_send(void *c, uint8_t code, uint8_t action) {
  fk_link_t *l = c;
  if (l->nsent < 512) {
    l->sent[l->nsent].code = code;
    l->sent[l->nsent++].action = action;
  }
  return 0;
}
static __attribute__((unused)) int fkl_poll(void *c, uint64_t now, app_link_event_t *ev) {
  fk_link_t *l = c;
  (void)now;
  if (l->qh >= l->nq) return 0;
  *ev = l->q[l->qh++];
  return 1;
}
static __attribute__((unused)) void fkl_set_awake(void *c, bool awake) {
  fk_link_t *l = c;
  l->awake = awake;
  l->nawake++;
}
static __attribute__((unused)) app_link_t fkl_link(fk_link_t *l) {
  return (app_link_t){.ctx = l, .send_key = fkl_send, .poll = fkl_poll, .set_awake = fkl_set_awake};
}

static __attribute__((unused)) void fkl_push_page(fk_link_t *l, const uint8_t *d, size_t n) {
  app_link_event_t *e = &l->q[l->nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_PAGE_PUSH;
  memcpy(e->data, d, n);
  e->len = n;
}
static __attribute__((unused)) void fkl_push_kind(fk_link_t *l, app_link_event_kind_t k) {
  app_link_event_t *e = &l->q[l->nq++];
  memset(e, 0, sizeof *e);
  e->kind = k;
}

/* Independent copy of the allow-list, applied to the syscall log (does not trust sys.c). */
static __attribute__((unused)) int fk_log_is_safe(const fk_t *f, const char *bootcount_path) {
  int ok = 1;
  for (int i = 0; i < f->nlog; i++) {
    const char *l = f->log[i];
    char path[96];
    int fl;
    if (sscanf(l, "open %95s %d", path, &fl) == 2) {
      int acc = fl & O_ACCMODE;
      int good = (!strcmp(path, "/dev/fb0") && fl == O_RDWR) ||
                 (!strcmp(path, "/dev/input/event0") && fl == O_RDONLY) ||
                 (!strcmp(path, "/dev/input/event2") && fl == O_RDONLY) ||
                 (!strcmp(path, "/sys/class/backlight/ap3156_backlight/brightness") && fl == O_WRONLY) ||
                 (!strcmp(path, "/sys/class/leds/tps6507x:white/brightness") && fl == O_WRONLY) ||
                 (!strcmp(path, "/sys/bus/i2c/devices/1-0070/soc") && fl == O_RDONLY) ||
                 (!strcmp(path, "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/powersource") && fl == O_RDONLY) ||
                 (!strcmp(path, "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/battcharge") && fl == O_RDONLY) ||
                 (bootcount_path && !strcmp(path, bootcount_path) && acc == O_WRONLY);
      if (!good) { fprintf(stderr, "unsafe syscall: %s\n", l); ok = 0; }
    } else if (!strncmp(l, "exec ", 5)) {
      if (strcmp(l + 5, "/sbin/poweroff")) { fprintf(stderr, "unsafe syscall: %s\n", l); ok = 0; }
    } else if (!strncmp(l, "ioctl ", 6)) {
      unsigned long req = strtoul(l + 6, NULL, 16);
      if (req != FBIOGET_VSCREENINFO && req != FBIOGET_FSCREENINFO && req != FBIOBLANK) { fprintf(stderr, "unsafe syscall: %s\n", l); ok = 0; }
    }
  }
  return ok;
}

static __attribute__((unused)) int fk_count(const fk_t *f, const char *prefix) {
  int n = 0;
  for (int i = 0; i < f->nlog; i++)
    if (!strncmp(f->log[i], prefix, strlen(prefix))) n++;
  return n;
}

#endif
