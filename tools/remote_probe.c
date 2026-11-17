/* remote_probe: READ-ONLY bring-up probe for the Elite remote (ARM926, kernel 2.6.37).
 * Experimental. Not part of the shipped remote.
 *
 * Safety: the only paths this tool will ever open are /dev/fb0 and /dev/input/event0..3, always
 * O_RDONLY, and the only ioctls are the read-only queries below. The allow-list lives in
 * rpr_open()/rpr_ioctl(), so no subcommand can bypass it. It never mmaps the framebuffer, never
 * writes anything, never grabs an input device (no EVIOCGRAB), and never names /dev/rffw,
 * /dev/mspfw, /dev/watchdog, /dev/mtd*, /dev/rfspi or anything under /sys. */
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/input.h>

#define RPR_FB "/dev/fb0"
#define RPR_NEVENT 4
#define RPR_KEYS_MAX_SECS 600

static const char *const RPR_EVENT[RPR_NEVENT] = {"/dev/input/event0", "/dev/input/event1", "/dev/input/event2",
                                                   "/dev/input/event3"};
static const unsigned RPR_ABS[] = {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TOOL_WIDTH};
static const char *const RPR_ABS_NAME[] = {"ABS_X", "ABS_Y", "ABS_PRESSURE", "ABS_TOOL_WIDTH"};
#define RPR_NABS (sizeof RPR_ABS / sizeof RPR_ABS[0])

/* Syscall seam. Failures return -1 with errno set. There is deliberately no write(). */
typedef struct rpr_ops {
  void *ctx;
  int (*open)(void *ctx, const char *path, int flags);
  int (*ioctl)(void *ctx, int fd, unsigned long req, void *arg);
  ssize_t (*read)(void *ctx, int fd, void *buf, size_t n);
  int (*close)(void *ctx, int fd);
  /* Wait up to timeout_ms for any of fds[0..n) to be readable; ready[i] set. Returns the number
   * ready, 0 on timeout, -1 on error (errno set; EINTR is retried by the caller). */
  int (*wait)(void *ctx, const int *fds, int n, unsigned timeout_ms, int *ready);
  uint64_t (*now_ms)(void *ctx);
} rpr_ops_t;

typedef struct { unsigned code; const char *name; } rpr_key_t;
static const rpr_key_t RPR_KEYS[] = {
    {103, "Up"}, {108, "Down"}, {105, "Left"}, {106, "Right"}, {352, "Select"}, {139, "Menu"},
    {174, "Exit"}, {115, "VolumeUp"}, {114, "VolumeDown"}, {113, "VolumeMute"}, {402, "ChannelUp"},
    {403, "ChannelDown"}, {412, "PrevChannel"}, {207, "Play"}, {119, "Pause"}, {128, "Stop"},
    {167, "Record"}, {168, "Rewind"}, {159, "FastForward"}, {389, "DVR"}, {395, "Guide"}, {358, "Info"},
    {398, "Red"}, {399, "Green"}, {400, "Yellow"}, {401, "Blue"}, {148, "HomeControl1"},
    {202, "HomeControl2"}, {149, "HomeControl3"}, {203, "HomeControl4"}, {78, "HomeControlPlus"},
    {74, "HomeControlMinus"}, {116, "PowerOffActivity"}, {102, "Home"}, {364, "Favorites"},
};

static const char *rpr_key_name(unsigned code) {
  for (size_t i = 0; i < sizeof RPR_KEYS / sizeof RPR_KEYS[0]; i++)
    if (RPR_KEYS[i].code == code) return RPR_KEYS[i].name;
  return "unknown";
}

static const char *rpr_abs_name(unsigned code) {
  for (size_t i = 0; i < RPR_NABS; i++)
    if (RPR_ABS[i] == code) return RPR_ABS_NAME[i];
  return "unknown";
}

/* ---- brick-safety allow-list ---- */

static int rpr_path_allowed(const char *p) {
  if (!strcmp(p, RPR_FB)) return 1;
  for (int i = 0; i < RPR_NEVENT; i++)
    if (!strcmp(p, RPR_EVENT[i])) return 1;
  return 0;
}

/* Read-only queries only: the two fb GETs, EVIOCGNAME, and EVIOCGABS for the four axes we name.
 * EVIOCGRAB, FBIOPUT_*, FBIOBLANK, FBIOPAN_DISPLAY and every EVIOCS* are refused. */
static int rpr_ioctl_allowed(unsigned long req) {
  if (req == FBIOGET_VSCREENINFO || req == FBIOGET_FSCREENINFO) return 1;
  if (_IOC_TYPE(req) != 'E' || _IOC_DIR(req) != _IOC_READ) return 0;
  if (_IOC_NR(req) == 0x06) return 1; /* EVIOCGNAME(len) */
  if (_IOC_SIZE(req) != sizeof(struct input_absinfo)) return 0;
  for (size_t i = 0; i < RPR_NABS; i++)
    if (_IOC_NR(req) == 0x40u + RPR_ABS[i]) return 1;
  return 0;
}

static int rpr_open(const rpr_ops_t *o, const char *path, int flags) {
  if (flags != O_RDONLY || !rpr_path_allowed(path)) { errno = EPERM; return -1; }
  return o->open(o->ctx, path, flags);
}

static int rpr_ioctl(const rpr_ops_t *o, int fd, unsigned long req, void *arg) {
  if (!rpr_ioctl_allowed(req)) { errno = EPERM; return -1; }
  return o->ioctl(o->ctx, fd, req, arg);
}

/* ---- output helpers ---- */

static void rpr_jstr(FILE *out, const char *s) {
  fputc('"', out);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
    else if (c < 0x20 || c >= 0x7F) fprintf(out, "\\u%04x", c);
    else fputc(c, out);
  }
  fputc('"', out);
}

/* ---- fbinfo ---- */

static int rpr_cmd_fbinfo(const rpr_ops_t *o, FILE *out, int json) {
  int fd = rpr_open(o, RPR_FB, O_RDONLY);
  if (fd < 0) {
    int e = errno;
    if (json) fprintf(out, "{\"cmd\":\"fbinfo\",\"ok\":false,\"errno\":\"%s\"}\n", strerror(e));
    else fprintf(out, "open %s O_RDONLY -> -1 errno=%s\n", RPR_FB, strerror(e));
    return 1;
  }
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  memset(&v, 0, sizeof v);
  memset(&f, 0, sizeof f);
  int rv = rpr_ioctl(o, fd, FBIOGET_VSCREENINFO, &v);
  int rf = rv < 0 ? -1 : rpr_ioctl(o, fd, FBIOGET_FSCREENINFO, &f);
  int e = errno;
  o->close(o->ctx, fd);
  if (rv < 0 || rf < 0) {
    if (json) fprintf(out, "{\"cmd\":\"fbinfo\",\"ok\":false,\"errno\":\"%s\"}\n", strerror(e));
    else fprintf(out, "fb ioctl failed: %s\n", strerror(e));
    return 1;
  }
#define BF(n) v.n.offset, v.n.length, v.n.msb_right
  if (json) {
    fprintf(out,
            "{\"cmd\":\"fbinfo\",\"ok\":true,\"xres\":%u,\"yres\":%u,\"xres_virtual\":%u,\"yres_virtual\":%u,"
            "\"bits_per_pixel\":%u,\"red\":{\"offset\":%u,\"length\":%u,\"msb_right\":%u},"
            "\"green\":{\"offset\":%u,\"length\":%u,\"msb_right\":%u},"
            "\"blue\":{\"offset\":%u,\"length\":%u,\"msb_right\":%u},"
            "\"transp\":{\"offset\":%u,\"length\":%u,\"msb_right\":%u},"
            "\"line_length\":%u,\"smem_len\":%u}\n",
            v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel, BF(red), BF(green), BF(blue),
            BF(transp), f.line_length, f.smem_len);
  } else {
    fprintf(out, "xres=%u yres=%u virtual=%ux%u bpp=%u\n", v.xres, v.yres, v.xres_virtual, v.yres_virtual,
            v.bits_per_pixel);
    fprintf(out, "red    offset=%u length=%u msb_right=%u\n", BF(red));
    fprintf(out, "green  offset=%u length=%u msb_right=%u\n", BF(green));
    fprintf(out, "blue   offset=%u length=%u msb_right=%u\n", BF(blue));
    fprintf(out, "transp offset=%u length=%u msb_right=%u\n", BF(transp));
    fprintf(out, "line_length=%u smem_len=%u\n", f.line_length, f.smem_len);
  }
#undef BF
  return 0;
}

/* ---- absinfo (event2) ---- */

static int rpr_cmd_absinfo(const rpr_ops_t *o, FILE *out, int json) {
  const char *path = RPR_EVENT[2];
  int fd = rpr_open(o, path, O_RDONLY);
  if (fd < 0) {
    int e = errno;
    if (json) fprintf(out, "{\"cmd\":\"absinfo\",\"ok\":false,\"errno\":\"%s\"}\n", strerror(e));
    else fprintf(out, "open %s O_RDONLY -> -1 errno=%s\n", path, strerror(e));
    return 1;
  }
  int bad = 0;
  if (json) fprintf(out, "{\"cmd\":\"absinfo\",\"device\":\"%s\",\"axes\":[", path);
  for (size_t i = 0; i < RPR_NABS; i++) {
    struct input_absinfo a;
    memset(&a, 0, sizeof a);
    int r = rpr_ioctl(o, fd, EVIOCGABS(RPR_ABS[i]), &a);
    int e = errno;
    bad |= r < 0;
    if (json) {
      fprintf(out, "%s{\"name\":\"%s\",\"code\":%u,", i ? "," : "", RPR_ABS_NAME[i], RPR_ABS[i]);
      if (r < 0) fprintf(out, "\"ok\":false,\"errno\":\"%s\"}", strerror(e));
      else fprintf(out, "\"ok\":true,\"min\":%d,\"max\":%d,\"fuzz\":%d,\"flat\":%d,\"resolution\":%d}", a.minimum,
                   a.maximum, a.fuzz, a.flat, a.resolution);
    } else if (r < 0) {
      fprintf(out, "%s: ioctl failed: %s\n", RPR_ABS_NAME[i], strerror(e));
    } else {
      fprintf(out, "%s: min=%d max=%d fuzz=%d flat=%d resolution=%d\n", RPR_ABS_NAME[i], a.minimum, a.maximum,
              a.fuzz, a.flat, a.resolution);
    }
  }
  if (json) fprintf(out, "]}\n");
  o->close(o->ctx, fd);
  return bad;
}

/* ---- name (event0..3) ---- */

static int rpr_cmd_name(const rpr_ops_t *o, FILE *out, int json) {
  int got = 0;
  if (json) fprintf(out, "{\"cmd\":\"name\",\"devices\":[");
  for (int i = 0; i < RPR_NEVENT; i++) {
    char name[256];
    memset(name, 0, sizeof name);
    int fd = rpr_open(o, RPR_EVENT[i], O_RDONLY);
    int e = errno, ok = 0;
    if (fd >= 0) {
      ok = rpr_ioctl(o, fd, EVIOCGNAME(sizeof name - 1), name) >= 0;
      e = errno;
      o->close(o->ctx, fd);
    }
    got += ok;
    if (json) {
      fprintf(out, "%s{\"path\":\"%s\",", i ? "," : "", RPR_EVENT[i]);
      if (ok) { fprintf(out, "\"ok\":true,\"name\":"); rpr_jstr(out, name); fputc('}', out); }
      else fprintf(out, "\"ok\":false,\"errno\":\"%s\"}", strerror(e));
    } else if (ok) {
      fprintf(out, "%s: \"%s\"\n", RPR_EVENT[i], name);
    } else {
      fprintf(out, "%s: unavailable (%s)\n", RPR_EVENT[i], strerror(e));
    }
  }
  if (json) fprintf(out, "]}\n");
  return got ? 0 : 1;
}

/* ---- keys (event0 + event2) ---- */

typedef struct { int fd; const char *path; const char *tag; } rpr_src_t;

/* Print one event if it is EV_KEY or EV_ABS; returns 1 if printed. */
static int rpr_print_event(FILE *out, int json, const char *tag, const struct input_event *e, int first) {
  if (e->type != EV_KEY && e->type != EV_ABS) return 0;
  const char *tn = e->type == EV_KEY ? "EV_KEY" : "EV_ABS";
  const char *cn = e->type == EV_KEY ? rpr_key_name(e->code) : rpr_abs_name(e->code);
  if (json) {
    fprintf(out, "%s{\"dev\":\"%s\",\"type\":\"%s\",\"code\":%u,\"name\":\"%s\",\"value\":%d}", first ? "" : ",", tag,
            tn, e->code, cn, e->value);
  } else {
    fprintf(out, "%s %s code=%u (%s) value=%d\n", tag, tn, e->code, cn, e->value);
    fflush(out);
  }
  return 1;
}

static int rpr_cmd_keys(const rpr_ops_t *o, unsigned secs, FILE *out, int json) {
  rpr_src_t src[2] = {{-1, RPR_EVENT[0], "event0"}, {-1, RPR_EVENT[2], "event2"}};
  int nopen = 0, bad = 0;
  unsigned long nprinted = 0;
  if (json) fprintf(out, "{\"cmd\":\"keys\",\"seconds\":%u,\"opened\":[", secs);
  for (int i = 0; i < 2; i++) {
    src[i].fd = rpr_open(o, src[i].path, O_RDONLY);
    int e = errno;
    if (src[i].fd >= 0) nopen++;
    if (json) fprintf(out, "%s{\"dev\":\"%s\",\"ok\":%s}", i ? "," : "", src[i].tag, src[i].fd >= 0 ? "true" : "false");
    else if (src[i].fd >= 0) fprintf(out, "%s: open O_RDONLY ok\n", src[i].path);
    else fprintf(out, "%s: unavailable (%s)\n", src[i].path, strerror(e));
  }
  if (json) fprintf(out, "],\"events\":[");
  if (!nopen) {
    if (json) fprintf(out, "],\"total\":0}\n");
    return 1;
  }
  if (!json) fprintf(out, "logging %us (passive; press keys, Ctrl-C to stop)\n", secs);
  uint64_t deadline = o->now_ms(o->ctx) + (uint64_t)secs * 1000u;
  for (;;) {
    int fds[2], idx[2], n = 0;
    for (int i = 0; i < 2; i++)
      if (src[i].fd >= 0) { fds[n] = src[i].fd; idx[n++] = i; }
    if (!n) break;
    uint64_t now = o->now_ms(o->ctx);
    if (now >= deadline) break;
    int ready[2] = {0, 0};
    int w = o->wait(o->ctx, fds, n, (unsigned)(deadline - now), ready);
    if (w < 0) { if (errno == EINTR) continue; bad = 1; break; }
    for (int k = 0; k < n && w > 0; k++) {
      if (!ready[k]) continue;
      struct input_event ev[8];
      ssize_t r = o->read(o->ctx, fds[k], ev, sizeof ev);
      if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        bad = 1; /* node went away: stop reading it, keep the other */
        o->close(o->ctx, src[idx[k]].fd);
        src[idx[k]].fd = -1;
        continue;
      }
      size_t cnt = (size_t)r / sizeof ev[0]; /* evdev returns whole events only */
      for (size_t j = 0; j < cnt; j++)
        nprinted += (unsigned long)rpr_print_event(out, json, src[idx[k]].tag, &ev[j], nprinted == 0);
    }
  }
  for (int i = 0; i < 2; i++) if (src[i].fd >= 0) o->close(o->ctx, src[i].fd);
  if (json) fprintf(out, "],\"total\":%lu}\n", nprinted);
  else fprintf(out, "events printed: %lu\n", nprinted);
  return bad;
}

static void rpr_usage(FILE *e) {
  fprintf(e, "usage: remote_probe (fbinfo | absinfo | name | keys SECONDS) [--json]\n"
             "  read-only: opens only /dev/fb0 and /dev/input/event0..3, O_RDONLY; never mmaps, grabs or writes\n"
             "  fbinfo   framebuffer var/fix screen info (pixel bitfields, line_length)\n"
             "  absinfo  EVIOCGABS on event2 for ABS_X/Y/PRESSURE/TOOL_WIDTH\n"
             "  name     EVIOCGNAME on event0..3\n"
             "  keys N   log EV_KEY/EV_ABS from event0 and event2 for N seconds (1..%d)\n", RPR_KEYS_MAX_SECS);
}

/* Whole tool, parameterised on the syscall seam; returns the exit code. */
static int rpr_run(const rpr_ops_t *o, int argc, char **argv, FILE *out, FILE *err) {
  enum { NONE, FBINFO, ABSINFO, NAME, KEYS } mode = NONE;
  int json = 0;
  unsigned secs = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--json")) json = 1;
    else if (mode == NONE && !strcmp(a, "fbinfo")) mode = FBINFO;
    else if (mode == NONE && !strcmp(a, "absinfo")) mode = ABSINFO;
    else if (mode == NONE && !strcmp(a, "name")) mode = NAME;
    else if (mode == NONE && !strcmp(a, "keys")) {
      mode = KEYS;
      char *end = NULL;
      long v = i + 1 < argc ? strtol(argv[i + 1], &end, 10) : 0;
      if (i + 1 >= argc || *argv[i + 1] == 0 || *end || v < 1 || v > RPR_KEYS_MAX_SECS) { rpr_usage(err); return 2; }
      secs = (unsigned)v;
      i++;
    } else { rpr_usage(err); return 2; }
  }
  if (mode == NONE) { rpr_usage(err); return 2; } /* safe no-op: nothing opened */
  return mode == FBINFO ? rpr_cmd_fbinfo(o, out, json)
       : mode == ABSINFO ? rpr_cmd_absinfo(o, out, json)
       : mode == NAME ? rpr_cmd_name(o, out, json)
       : rpr_cmd_keys(o, secs, out, json);
}

#ifndef RPR_TEST
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int r_open(void *c, const char *p, int fl) { (void)c; return open(p, fl); }
static int r_ioctl(void *c, int fd, unsigned long req, void *a) { (void)c; return ioctl(fd, req, a); }
static ssize_t r_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static int r_close(void *c, int fd) { (void)c; return close(fd); }
static uint64_t r_now(void *c) {
  (void)c;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int r_wait(void *c, const int *fds, int n, unsigned ms, int *ready) {
  (void)c;
  fd_set s;
  FD_ZERO(&s);
  int mx = -1;
  for (int i = 0; i < n; i++) { FD_SET(fds[i], &s); if (fds[i] > mx) mx = fds[i]; }
  struct timeval tv = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000};
  int r = select(mx + 1, &s, NULL, NULL, &tv);
  if (r > 0) for (int i = 0; i < n; i++) ready[i] = FD_ISSET(fds[i], &s) != 0;
  return r;
}

int main(int argc, char **argv) {
  rpr_ops_t ops = {NULL, r_open, r_ioctl, r_read, r_close, r_wait, r_now};
  return rpr_run(&ops, argc, argv, stdout, stderr);
}
#endif
