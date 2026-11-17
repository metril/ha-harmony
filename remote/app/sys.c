#include "remote/app/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void ra_sys_init(ra_sys_t *s, const ra_ops_t *ops, const char *bootcount_path) {
  s->ops = *ops;
  s->bootcount_path = bootcount_path && *bootcount_path ? bootcount_path : NULL;
  s->fb_fd = -1;
}

static int eperm(void) {
  errno = EPERM;
  return -1;
}

static int path_is(const char *a, const char *b) { return !strcmp(a, b); }

int ra_open_fb(ra_sys_t *s) {
  int fd = s->ops.open(s->ops.ctx, RA_PATH_FB, O_RDWR, 0);
  if (fd >= 0) s->fb_fd = fd;
  return fd;
}

int ra_open_input(ra_sys_t *s, const char *p) {
  if (!path_is(p, RA_PATH_KEYS) && !path_is(p, RA_PATH_TOUCH)) return eperm();
  return s->ops.open(s->ops.ctx, p, O_RDONLY, 0);
}

int ra_ioctl_fb(ra_sys_t *s, unsigned long req, void *arg) {
  if (s->fb_fd < 0 || (req != FBIOGET_VSCREENINFO && req != FBIOGET_FSCREENINFO)) return eperm();
  return s->ops.ioctl(s->ops.ctx, s->fb_fd, req, arg);
}

int ra_fb_unblank(ra_sys_t *s) {
  if (s->fb_fd < 0) return eperm();
  return s->ops.ioctl(s->ops.ctx, s->fb_fd, FBIOBLANK, (void *)(intptr_t)FB_BLANK_UNBLANK);
}

void *ra_mmap_fb(ra_sys_t *s, size_t len) {
  if (s->fb_fd < 0) {
    errno = EPERM;
    return NULL;
  }
  return s->ops.mmap(s->ops.ctx, s->fb_fd, len);
}

ssize_t ra_read(ra_sys_t *s, int fd, void *buf, size_t n) { return s->ops.read(s->ops.ctx, fd, buf, n); }
int ra_close(ra_sys_t *s, int fd) { return s->ops.close(s->ops.ctx, fd); }

int ra_sysfs_write(ra_sys_t *s, const char *path, const char *val) {
  if (!path_is(path, RA_PATH_BACKLIGHT) && !path_is(path, RA_PATH_LED)) return eperm();
  int fd = s->ops.open(s->ops.ctx, path, O_WRONLY, 0);
  if (fd < 0) return -1;
  size_t n = strlen(val);
  ssize_t w = s->ops.write(s->ops.ctx, fd, val, n);
  int e = errno;
  s->ops.close(s->ops.ctx, fd);
  if (w != (ssize_t)n) {
    errno = w < 0 ? e : EIO;
    return -1;
  }
  return 0;
}

int ra_sysfs_read(ra_sys_t *s, const char *path, char *buf, size_t n) {
  if (!path_is(path, RA_PATH_SOC) && !path_is(path, RA_PATH_POWERSRC) && !path_is(path, RA_PATH_BATTCHARGE))
    return eperm();
  if (n < 2) return eperm();
  int fd = s->ops.open(s->ops.ctx, path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t r = s->ops.read(s->ops.ctx, fd, buf, n - 1);
  int e = errno;
  s->ops.close(s->ops.ctx, fd);
  if (r < 0) {
    errno = e;
    return -1;
  }
  buf[r] = 0;
  return (int)r;
}

int ra_write_bootcount(ra_sys_t *s) {
  if (!s->bootcount_path) return 0;
  int fd = s->ops.open(s->ops.ctx, s->bootcount_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  ssize_t w = s->ops.write(s->ops.ctx, fd, "0\n", 2);
  s->ops.close(s->ops.ctx, fd);
  return w == 2 ? 0 : -1;
}

int ra_poweroff(ra_sys_t *s) { return s->ops.exec_poweroff(s->ops.ctx, RA_POWEROFF_PATH); }

void ra_logf(ra_sys_t *s, const char *fmt, ...) {
  char m[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof m, fmt, ap);
  va_end(ap);
  if (s->ops.log) s->ops.log(s->ops.ctx, m);
}
