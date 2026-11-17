/* The real syscall table (Linux). Everything else in the app sees only ra_ops_t. */
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "remote/app/sys.h"

static int r_open(void *c, const char *p, int fl, int mode) { (void)c; return open(p, fl, mode); }
static int r_close(void *c, int fd) { (void)c; return close(fd); }
static ssize_t r_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static ssize_t r_write(void *c, int fd, const void *b, size_t n) { (void)c; return write(fd, b, n); }
static int r_ioctl(void *c, int fd, unsigned long rq, void *a) { (void)c; return ioctl(fd, rq, a); }

static void *r_mmap(void *c, int fd, size_t len) {
  (void)c;
  void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  return p == MAP_FAILED ? NULL : p;
}

static int r_munmap(void *c, void *p, size_t len) { (void)c; return munmap(p, len); }

static int r_wait(void *c, const int *fds, int n, unsigned ms, int *ready) {
  (void)c;
  fd_set rs;
  FD_ZERO(&rs);
  int mx = -1;
  for (int i = 0; i < n; i++) {
    FD_SET(fds[i], &rs);
    if (fds[i] > mx) mx = fds[i];
  }
  struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  int r = select(mx + 1, &rs, NULL, NULL, &tv);
  if (r > 0)
    for (int i = 0; i < n; i++) ready[i] = FD_ISSET(fds[i], &rs) ? 1 : 0;
  return r;
}

static uint64_t r_now(void *c) {
  (void)c;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int r_poweroff(void *c, const char *path) {
  (void)c;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execl(path, path, (char *)NULL);
    _exit(127);
  }
  return 0;
}

static void r_log(void *c, const char *m) { (void)c; fprintf(stderr, "harmony_remote: %s\n", m); }

const ra_ops_t ra_real_ops = {
    .open = r_open, .close = r_close, .read = r_read, .write = r_write, .ioctl = r_ioctl,
    .mmap = r_mmap, .munmap = r_munmap, .wait = r_wait, .now_ms = r_now,
    .exec_poweroff = r_poweroff, .log = r_log,
};
