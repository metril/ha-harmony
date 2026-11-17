/* rf_probe: READ-ONLY probe of the hub's CC2544 radio via /dev/rfspi.
 * Experimental. Not part of the shipped hub.
 *
 * Safety: opens only /dev/rfspi (never /dev/rffw, the flasher), writes only the allow-listed
 * query frames below (no pairing, unpair or power-mode command), and writes no file. The
 * allow-list is enforced in rfp_send(), so no subcommand can bypass it.
 *
 * /dev/rfspi has no ioctls. write() is a full-duplex SPI transaction whose return value is the
 * RECEIVED frame length (0 = nothing came back), never the bytes written. read() ignores its
 * count (no kernel bounds check), blocks until a frame is queued, and has no timeout: every read
 * is bounded with alarm()/SIGALRM, as stock hal does, and gets a >= 64 byte buffer. */
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/hp_errors.h"

#define RFP_DEV "/dev/rfspi"
/* The driver's queued entry is a fixed 64-byte buffer with its length at +0x40, and read() ignores
 * the caller's `count` entirely -- it copies `len` bytes whatever we ask for. So a 64-byte buffer
 * has no headroom: a wrong length field would overrun the stack instead of being observable. Give
 * read() twice what the driver can legitimately hold, keep the canary over the whole thing, and
 * treat anything past RFP_RXMAX as a driver bug worth reporting rather than a crash. */
#define RFP_BUF 128
#define RFP_RXMAX 64
#define RFP_CANARY 0xAA
#define RFP_CMD_LEN 7

static const uint8_t RFP_INIT[RFP_CMD_LEN] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
static const uint8_t RFP_VER1[RFP_CMD_LEN] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
static const uint8_t RFP_VER2[RFP_CMD_LEN] = {0x10, 0xFF, 0x81, 0xF1, 0x02, 0x00, 0x00};
static const uint8_t RFP_PIPES[] = {0x03, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x30, 0x31,
                                    0x32, 0x33, 0x34, 0x35, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45};
#define RFP_NPIPES (sizeof RFP_PIPES / sizeof RFP_PIPES[0])

/* Syscall seam (same shape as hp_hw_ops_t). Failures return -1 with errno set. A read bounded
 * by alarm() that times out returns -1/EINTR. */
typedef struct rfp_ops {
  void *ctx;
  int (*open)(void *ctx, const char *path, int flags);
  ssize_t (*write)(void *ctx, int fd, const void *buf, size_t n);
  ssize_t (*read)(void *ctx, int fd, void *buf, size_t cap);
  int (*close)(void *ctx, int fd);
  void (*alarm)(void *ctx, unsigned secs);
  /* 1 = a process named `hal` is alive, 0 = not, -1 = could not tell. See rfp_hal_alive(). */
  int (*hal_alive)(void *ctx);
} rfp_ops_t;

/* Frame length the chip clocks back, chosen by the first byte it returns; 0 = none/unknown. */
static size_t rfp_frame_len(uint8_t first) {
  switch (first) {
  case 0x01: case 0x02: case 0x10: return 7;
  case 0x03: return 4;
  case 0x04: case 0x08: return 1;
  case 0x11: return 20;
  case 0x12: return 30;
  case 0x20: return 32;
  default: return 0;
  }
}

static void rfp_pipe_cmd(uint8_t pipe, uint8_t out[RFP_CMD_LEN]) {
  static const uint8_t t[RFP_CMD_LEN] = {0x10, 0xFF, 0x83, 0xB5, 0, 0x00, 0x00};
  memcpy(out, t, RFP_CMD_LEN);
  out[4] = pipe;
}

/* Allow-list: the init frame, the two version queries, and pipe-table queries. Nothing else may
 * reach write(): pairing (B2), unpair, power-mode (first byte 0x20) all fail here. */
static int rfp_cmd_allowed(const uint8_t *c, size_t n) {
  if (n != RFP_CMD_LEN) return 0;
  if (!memcmp(c, RFP_INIT, n) || !memcmp(c, RFP_VER1, n) || !memcmp(c, RFP_VER2, n)) return 1;
  if (c[0] == 0x10 && c[1] == 0xFF && c[2] == 0x83 && c[3] == 0xB5 && c[5] == 0 && c[6] == 0) {
    for (size_t i = 0; i < RFP_NPIPES; i++)
      if (c[4] == RFP_PIPES[i]) return 1;
  }
  return 0;
}

/* Number of bytes at or beyond `len` that no longer hold the canary (kernel overran the reply). */
static size_t rfp_canary_dirty(const uint8_t *buf, size_t cap, size_t len) {
  size_t d = 0;
  for (size_t i = len < cap ? len : cap; i < cap; i++) d += buf[i] != RFP_CANARY;
  return d;
}

static void rfp_hex(const uint8_t *b, size_t n, char *out, size_t cap) {
  size_t o = 0;
  if (cap) out[0] = 0;
  for (size_t i = 0; i < n && o + 4 < cap; i++) o += (size_t)snprintf(out + o, cap - o, i ? " %02X" : "%02X", b[i]);
}

typedef struct {
  uint8_t tx[RFP_CMD_LEN];
  int wret;               /* write() return: RECEIVED length, or -1 */
  uint8_t rx[RFP_BUF];
  int rxlen;
  int skipped;            /* non-matching frames read and dropped */
  int status;             /* 0 ok, -HP_E_NOENT timeout, -HP_E_IO, -HP_E_INVAL (not allowed) */
} rfp_xfer_t;

/* Send one allow-listed frame and wait (1 s alarm) for the reply whose bytes [from..4] equal the
 * command's. from=0 is stock hal's exact "first 5 bytes" match; from=1 ignores the report id
 * (pipe replies come back as 0x11 for a 0x10 command). With wait=0 only the write is done. */
static void rfp_xfer(const rfp_ops_t *o, int fd, const uint8_t *cmd, size_t from, int wait, rfp_xfer_t *x) {
  memset(x, 0, sizeof *x);
  x->wret = -1;
  if (!rfp_cmd_allowed(cmd, RFP_CMD_LEN)) { x->status = -HP_E_INVAL; return; }
  memcpy(x->tx, cmd, RFP_CMD_LEN);
  x->wret = (int)o->write(o->ctx, fd, cmd, RFP_CMD_LEN); /* NOT a bytes-written count */
  if (x->wret < 0) { x->status = -HP_E_IO; return; }
  if (!wait) return;
  x->status = -HP_E_NOENT;
  o->alarm(o->ctx, 1);
  for (;;) {
    uint8_t buf[RFP_BUF];
    memset(buf, RFP_CANARY, sizeof buf);
    ssize_t n = o->read(o->ctx, fd, buf, sizeof buf);
    if (n < 0) { if (errno != EINTR) x->status = -HP_E_IO; break; } /* EINTR = alarm = timeout */
    if (n >= 5 && (size_t)n <= sizeof buf && !memcmp(buf + from, cmd + from, 5 - from)) {
      memcpy(x->rx, buf, (size_t)n);
      x->rxlen = (int)n;
      x->status = 0;
      break;
    }
    x->skipped++;
  }
  o->alarm(o->ctx, 0);
}

static void rfp_jstr(FILE *out, const char *k, const char *v, int comma) {
  fprintf(out, "\"%s\":\"%s\"%s", k, v, comma ? "," : "");
}

static void rfp_print_xfer(FILE *out, const char *label, const rfp_xfer_t *x, int json) {
  char t[64], r[RFP_BUF * 3 + 4];
  rfp_hex(x->tx, RFP_CMD_LEN, t, sizeof t);
  rfp_hex(x->rx, (size_t)x->rxlen, r, sizeof r);
  const char *st = x->status == 0 ? "ok" : x->status == -HP_E_NOENT ? "timeout" : x->status == -HP_E_IO ? "io-error" : "refused";
  if (json) {
    fprintf(out, "{\"label\":\"%s\",", label);
    rfp_jstr(out, "tx", t, 1);
    fprintf(out, "\"write_ret\":%d,", x->wret);
    rfp_jstr(out, "rx", r, 1);
    fprintf(out, "\"rx_len\":%d,\"skipped\":%d,", x->rxlen, x->skipped);
    rfp_jstr(out, "status", st, 0);
    fputc('}', out);
    return;
  }
  fprintf(out, "%s: tx=[%s] write()=%d (rx frame length, not bytes written)\n", label, t, x->wret);
  fprintf(out, "%s: read len=%d rx=[%s] skipped=%d status=%s\n", label, x->rxlen, r, x->skipped, st);
}

static int rfp_cmd_probe(const rfp_ops_t *o, int fd, FILE *out, int json) {
  (void)o; (void)fd;
  if (json) fprintf(out, "{\"cmd\":\"probe\",\"device\":\"%s\",\"free\":true}\n", RFP_DEV);
  else fprintf(out, "open %s O_RDWR -> ok\ndevice is free (stock hal is not holding it)\n", RFP_DEV);
  return 0;
}

static int rfp_cmd_version(const rfp_ops_t *o, int fd, FILE *out, int json) {
  rfp_xfer_t xi, x1, x2;
  rfp_xfer(o, fd, RFP_INIT, 0, 1, &xi); /* init reply is informational only */
  rfp_xfer(o, fd, RFP_VER1, 0, 1, &x1);
  rfp_xfer(o, fd, RFP_VER2, 0, 1, &x2);
  int ok = x1.status == 0 && x2.status == 0 && x1.rxlen >= 7 && x2.rxlen >= 7;
  unsigned maj = ok ? x1.rx[5] : 0, min = ok ? x1.rx[6] : 0;
  unsigned bbe = ok ? (unsigned)(x2.rx[5] << 8 | x2.rx[6]) : 0, ble = ok ? (unsigned)(x2.rx[6] << 8 | x2.rx[5]) : 0;
  if (json) {
    fprintf(out, "{\"cmd\":\"version\",\"xfers\":[");
    rfp_print_xfer(out, "init", &xi, 1); fputc(',', out);
    rfp_print_xfer(out, "ver1", &x1, 1); fputc(',', out);
    rfp_print_xfer(out, "ver2", &x2, 1);
    fprintf(out, "],\"decoded\":%s", ok ? "true" : "false");
    if (ok) fprintf(out, ",\"major\":%u,\"minor\":%u,\"build_be\":%u,\"build_le\":%u", maj, min, bbe, ble);
    fprintf(out, ",\"stock_version_file\":\"39.00.0010\"}\n");
  } else {
    rfp_print_xfer(out, "init", &xi, 0);
    rfp_print_xfer(out, "ver1", &x1, 0);
    rfp_print_xfer(out, "ver2", &x2, 0);
    if (ok) fprintf(out, "decoded: %02u.%02u.%04u (build big-endian; little-endian %04u)\n", maj, min, bbe, ble);
    else fprintf(out, "decoded: unavailable (a version query got no matching reply)\n");
    fprintf(out, "stock /lib/firmware/cc2544.version: 39.00.0010 (compare)\n");
  }
  return ok ? 0 : 1;
}

static const char *rfp_pipe_meaning(uint8_t p) {
  if (p == 0x03) return "hub RF address (4 B)";
  if (p >= 0x20 && p <= 0x25) return "EquadID (0x4021 = Juniper family, incl. Elite)";
  if (p >= 0x30 && p <= 0x35) return "remote RF address (4 B)";
  return "remote name";
}

static int rfp_cmd_pipes(const rfp_ops_t *o, int fd, FILE *out, int json) {
  int bad = 0;
  if (json) fprintf(out, "{\"cmd\":\"pipes\",\"pipes\":[");
  for (size_t i = 0; i < RFP_NPIPES; i++) {
    uint8_t c[RFP_CMD_LEN];
    rfp_xfer_t x;
    char lbl[32];
    rfp_pipe_cmd(RFP_PIPES[i], c);
    rfp_xfer(o, fd, c, 1, 1, &x);
    snprintf(lbl, sizeof lbl, "pipe 0x%02X", RFP_PIPES[i]);
    bad |= x.status != 0;
    if (json) {
      if (i) fputc(',', out);
      rfp_print_xfer(out, lbl, &x, 1);
    } else {
      fprintf(out, "[%s]\n", rfp_pipe_meaning(RFP_PIPES[i]));
      rfp_print_xfer(out, lbl, &x, 0);
    }
  }
  if (json) fprintf(out, "]}\n");
  return bad;
}

static int rfp_cmd_listen(const rfp_ops_t *o, int fd, unsigned secs, FILE *out, int json) {
  unsigned counts[256] = {0}, nframes = 0, overruns = 0;
  int bad = 0;
  if (json) fprintf(out, "{\"cmd\":\"listen\",\"seconds\":%u,\"frames\":[", secs);
  else fprintf(out, "listening %us (passive; Ctrl-C to stop)\n", secs);
  o->alarm(o->ctx, secs);
  for (;;) {
    uint8_t buf[RFP_BUF];
    char h[64];
    memset(buf, RFP_CANARY, sizeof buf);
    ssize_t n = o->read(o->ctx, fd, buf, sizeof buf);
    if (n < 0) { if (errno != EINTR) bad = 1; break; } /* EINTR: SIGALRM or SIGINT */
    size_t len = (size_t)n;
    size_t dirty = rfp_canary_dirty(buf, sizeof buf, len);
    rfp_hex(buf, len < 8 ? len : 8, h, sizeof h);
    if (len) counts[buf[0]]++;
    /* Past RFP_RXMAX the driver has exceeded its own entry buffer, which it should never do. */
    if (dirty || len > RFP_RXMAX) overruns++;
    if (json) fprintf(out, "%s{\"len\":%zu,\"head\":\"%s\",\"beyond_len_changed\":%zu}", nframes ? "," : "", len, h, dirty);
    else fprintf(out, "frame %u: len=%zu (table says %zu) head=[%s] bytes-beyond-len-changed=%zu%s\n", nframes, len,
                 len ? rfp_frame_len(buf[0]) : 0, h, dirty,
                 dirty ? " (KERNEL WROTE PAST RETURNED LENGTH)" : "");
    nframes++;
  }
  o->alarm(o->ctx, 0);
  if (json) fprintf(out, "],\"counts\":{");
  else fprintf(out, "frames=%u overrun_frames=%u (%s)\n", nframes, overruns,
               overruns ? "kernel wrote beyond the returned length" : "no write beyond returned length observed");
  int first = 1;
  for (int i = 0; i < 256; i++) {
    if (!counts[i]) continue;
    if (json) fprintf(out, "%s\"0x%02X\":%u", first ? "" : ",", i, counts[i]);
    else fprintf(out, "report 0x%02X: %u\n", i, counts[i]);
    first = 0;
  }
  if (json) fprintf(out, "},\"total\":%u,\"overrun_frames\":%u}\n", nframes, overruns);
  return bad;
}

static void rfp_usage(FILE *e) {
  fprintf(e, "usage: rf_probe (--probe | --version | --pipes | --listen [SECONDS]) [--json]\n"
             "  read-only: queries the CC2544 via /dev/rfspi; never touches /dev/rffw, never pairs\n"
             "  --probe    open /dev/rfspi and close (is stock hal holding it?); sends nothing\n"
             "  --version  init + version queries; compare with 39.00.0010\n"
             "  --pipes    dump the pairing table (query only)\n"
             "  --listen   passively read frames (default 60 s), tally report ids, check overruns\n");
}

/* Whole tool, parameterised on the syscall seam; returns the exit code. */
static int rfp_run(const rfp_ops_t *o, int argc, char **argv, FILE *out, FILE *err) {
  enum { NONE, PROBE, VERSION, PIPES, LISTEN } mode = NONE;
  int json = 0;
  unsigned secs = 60;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--probe")) mode = PROBE;
    else if (!strcmp(a, "--version")) mode = VERSION;
    else if (!strcmp(a, "--pipes")) mode = PIPES;
    else if (!strcmp(a, "--json")) json = 1;
    else if (!strcmp(a, "--listen")) {
      mode = LISTEN;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        long v = strtol(argv[++i], NULL, 10);
        if (v < 1 || v > 86400) { rfp_usage(err); return 2; }
        secs = (unsigned)v;
      }
    } else { rfp_usage(err); return 2; }
  }
  if (mode == NONE) { rfp_usage(err); return 2; } /* safe no-op */
  /* Unlike /dev/i2s, the cc2544 driver's open() is NOT exclusive: it only bumps a counter, so it
   * never returns EBUSY and the errno check below can never catch stock hal holding the node. We
   * would simply open it and then contend with hal on the SPI bus the radio shares with NOR flash,
   * which is the documented way to hang that bus. So refuse up front, and refuse too when the
   * check itself cannot answer -- on this device there is no safe way to find out afterwards. */
  int halive = o->hal_alive ? o->hal_alive(o->ctx) : -1;
  if (halive != 0) {
    const char *why = halive > 0 ? "stock hal is alive" : "could not determine whether stock hal is alive";
    if (json)
      fprintf(out, "{\"cmd\":\"preflight\",\"ok\":false,\"hal_alive\":%s,\"reason\":\"%s\"}\n",
              halive > 0 ? "true" : "null", why);
    else
      fprintf(err, "refusing to open %s: %s.\nRun `tools/deploy_hub.py quiesce --yes` first; opening the\n"
                   "radio while hal holds it can hang the shared flash SPI bus.\n", RFP_DEV, why);
    return 1;
  }
  int fd = o->open(o->ctx, RFP_DEV, 2 /* O_RDWR */);
  if (fd < 0) {
    int e = errno;
    if (json) fprintf(out, "{\"cmd\":\"open\",\"device\":\"%s\",\"ok\":false,\"errno\":\"%s\",\"held\":%s}\n", RFP_DEV, strerror(e),
                      e == EBUSY || e == EACCES ? "true" : "false");
    else {
      fprintf(out, "open %s O_RDWR -> -1 errno=%s\n", RFP_DEV, strerror(e));
      if (e == EBUSY || e == EACCES) fprintf(out, "device is held -- stock hal still has it; quiesce first\n");
    }
    return 1;
  }
  int rc = mode == PROBE ? rfp_cmd_probe(o, fd, out, json)
         : mode == VERSION ? rfp_cmd_version(o, fd, out, json)
         : mode == PIPES ? rfp_cmd_pipes(o, fd, out, json)
         : rfp_cmd_listen(o, fd, secs, out, json);
  o->close(o->ctx, fd);
  return rc;
}

#ifndef RFP_TEST
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static void on_sig(int s) { (void)s; } /* only to interrupt read() with EINTR */

static int r_open(void *c, const char *p, int fl) { (void)c; return open(p, fl); }
static ssize_t r_write(void *c, int fd, const void *b, size_t n) { (void)c; return write(fd, b, n); }
static ssize_t r_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static int r_close(void *c, int fd) { (void)c; return close(fd); }
static void r_alarm(void *c, unsigned s) { (void)c; alarm(s); }

/* Is a process named exactly `hal` alive? Walk /proc/<pid>/stat and read the name between the
 * first '(' and the last ')' -- the hub's 2.6.31 kernel has no /proc/<pid>/comm (added in 2.6.33),
 * and its BusyBox has no pgrep. Returns 1 yes, 0 no, -1 if /proc could not be read at all; an
 * unreadable individual entry is skipped, since a pid can exit mid-walk. Matches the name
 * deploy_hub.py's named_alive("hal") uses. */
static int r_hal_alive(void *c) {
  (void)c;
  DIR *d = opendir("/proc");
  if (!d) return -1;
  int found = 0;
  struct dirent *e;
  while (!found && (e = readdir(d))) {
    const char *n = e->d_name;
    size_t i = 0;
    while (n[i] >= '0' && n[i] <= '9') i++;
    if (i == 0 || n[i] != 0) continue; /* not a pid directory */
    char path[64], buf[256];
    if (snprintf(path, sizeof path, "/proc/%s/stat", n) >= (int)sizeof path) continue;
    int fd = open(path, O_RDONLY);
    if (fd < 0) continue;
    ssize_t got = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (got <= 0) continue;
    buf[got] = 0;
    char *a = strchr(buf, '('), *b = strrchr(buf, ')');
    if (!a || !b || b <= a + 1) continue;
    *b = 0;
    found = strcmp(a + 1, "hal") == 0;
  }
  closedir(d);
  return found;
}

int main(int argc, char **argv) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig; /* no SA_RESTART: read() must return EINTR */
  sigaction(SIGALRM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  rfp_ops_t ops = {NULL, r_open, r_write, r_read, r_close, r_alarm, r_hal_alive};
  return rfp_run(&ops, argc, argv, stdout, stderr);
}
#endif
