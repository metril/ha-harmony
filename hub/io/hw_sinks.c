#define _POSIX_C_SOURCE 200809L
#include "hub/io/hw_sinks.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "hub/bt_link.h"
#include "hub/hp_errors.h"

/* --- real syscalls ------------------------------------------------------------------------ */

static int real_open(void *c, const char *p, int f) { (void)c; return open(p, f); }
static ssize_t real_write(void *c, int fd, const void *b, size_t n) { (void)c; return write(fd, b, n); }
static int real_ioctl(void *c, int fd, unsigned long r, unsigned long a) { (void)c; return ioctl(fd, r, a); }
static int real_close(void *c, int fd) { (void)c; return close(fd); }
static uint64_t real_now(void *c) {
    (void)c;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static const hp_hw_ops_t REAL_OPS = {NULL, real_open, real_write, real_ioctl, real_close, real_now};

/* --- bitstream ---------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap_bits, nbits;
    bool over;
} bits_t;

static void put_bit(bits_t *b, int v) {
    if (b->nbits >= b->cap_bits) { b->over = true; return; }
    if (b->nbits % 8 == 0) b->buf[b->nbits / 8] = 0;
    if (v) b->buf[b->nbits / 8] |= (uint8_t)(0x80 >> (b->nbits % 8));
    b->nbits++;
}

static void pad16(bits_t *b) {
    while (b->nbits % 16 && !b->over) put_bit(b, 0);
}

/* Same algorithm as tools/ir_bench.c (bench-proven); the tests diff the two byte-for-byte. */
int hp_hw_ir_build(const uint16_t *us, size_t n, uint32_t hz, unsigned repeat,
                   uint8_t *buf, size_t cap, hp_hw_wave_t *w) {
    if (!us || !n || !buf || !w || repeat < 1 || hz < 9000 || hz > 500000) return -HP_E_INVAL;
    const uint32_t duty = 33;
    uint32_t X = (1000000000u + hz / 2) / hz;
    uint32_t N = X < 2251 ? 2 : X < 2501 ? 4 : X < 3000 ? 8 : X < 12000 ? 10 : 12;
    uint32_t b = (X - 1 + N) / N;
    uint32_t pulse = (duty * N + 99) / 100;
    bits_t bs = {buf, cap * 8, 0, false};
    for (unsigned r = 0; r < repeat && !bs.over; r++) {
        uint64_t acc = 0, target = 0; /* ns: emitted vs wanted, so rounding never accumulates */
        for (size_t e = 0; e < n && !bs.over; e++) {
            bool mark = (e % 2) == 0;
            target += (uint64_t)us[e] * 1000u;
            while (acc + X / 2 < target && !bs.over) {
                for (uint32_t k = 0; k < N; k++) put_bit(&bs, mark && k < pulse);
                acc += X;
            }
        }
        pad16(&bs);
    }
    for (int k = 0; k < 32; k++) put_bit(&bs, 0);
    pad16(&bs);
    if (bs.over) return -HP_E_TOOBIG;
    size_t nbytes = bs.nbits / 8;
    for (size_t i = 0; i + 1 < nbytes; i += 2) { /* 16-bit words, adjacent byte pairs swapped */
        uint8_t t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
    }
    w->period_ns = X;
    w->n = N;
    w->pulse = pulse;
    w->bit_ns = b;
    w->divider = (uint32_t)(((uint64_t)b << 16) / 40u);
    w->nbits = bs.nbits;
    w->nbytes = nbytes;
    w->dur_ms = (uint32_t)(((uint64_t)bs.nbits * b + 999999u) / 1000000u);
    return 0;
}

/* --- device sequence ---------------------------------------------------------------------- */

static int ioc(hp_hw_t *h, unsigned long req, unsigned long arg) {
    for (int i = 0; i < 100; i++) {
        if (h->ops.ioctl(h->ops.ctx, h->fd, req, arg) >= 0) return 0;
        if (errno != EINTR) return -HP_E_IO;
    }
    return -HP_E_IO;
}

/* DRAIN the previous waveform. The fd is never closed: stock hal holds /dev/i2s for its whole life, and closing
 * frees the DMA ring while IRQ 23 can still fire (hard lockup). */
static int drain_prev(hp_hw_t *h) {
    if (h->flush_ports) { /* a failed send left bytes queued: no reset/flush ioctl exists, so play them out */
        int rc = ioc(h, HP_I2S_START, (unsigned long)((uint32_t)h->flush_ports << 16));
        if (rc < 0) return rc; /* keep flush_ports: DRAIN without a START can sleep forever; retry next time */
        h->flush_ports = 0;
        h->pending = true;
    }
    if (!h->pending) return 0;
    (void)ioc(h, HP_I2S_DRAIN, 0); /* normally returns at once: irout books the line for the waveform */
    h->pending = false;
    return 0;
}

static int open_dev(hp_hw_t *h) {
    const char *p = h->cfg.dev_path ? h->cfg.dev_path : HP_HW_DEFAULT_DEV;
    int fd;
    do fd = h->ops.open(h->ops.ctx, p, O_WRONLY);
    while (fd < 0 && errno == EINTR);
    if (fd >= 0) return fd;
    if (errno == EBUSY || errno == EACCES) return -HP_E_HALBUSY;
    return errno == ENOENT || errno == ENODEV ? -HP_E_NOENT : -HP_E_IO;
}

/* Port mapping: see the header comment. Returns START's `ports` byte, or -HP_E_INVAL. */
static int map_ports(const hp_hw_t *h, uint8_t port_mask) {
    uint8_t allowed = h->cfg.ir_ports;
    uint8_t eff = port_mask ? (uint8_t)(port_mask & allowed) : allowed;
    return eff ? eff : -HP_E_INVAL;
}

static int ir_send(void *self, uint8_t port_mask, const hp_ir_signal_t *sig) {
    hp_hw_t *h = self;
    if (!sig || sig->once_n == 0 || sig->once_n > HP_IR_MAX_DUR) return -HP_E_INVAL;
    int ports = map_ports(h, port_mask);
    if (ports < 0) return ports;
    hp_hw_wave_t w;
    int rc = hp_hw_ir_build(sig->us, sig->once_n, sig->carrier_hz, 1, h->buf, sizeof h->buf, &w);
    if (rc < 0) return rc; /* too big for the ring: reject, nothing touches the device */

    if (h->fd >= 0) {
        if ((rc = drain_prev(h)) < 0) return rc;
    } else {
        int fd = open_dev(h); /* first send (or retry after a failed open); then held for the daemon's life */
        if (fd < 0) return fd;
        h->fd = fd;
        h->configured = false;
    }
    if (!h->configured) {
        if ((rc = ioc(h, HP_I2S_DSIZE, 16)) < 0 || (rc = ioc(h, HP_I2S_MODE, 2)) < 0 ||
            (rc = ioc(h, HP_I2S_VOLUME, 15)) < 0)
            return rc;
        h->configured = true;
    }
    if ((rc = ioc(h, HP_I2S_FREQ, w.divider)) < 0) return rc;
    for (size_t off = 0; off < w.nbytes;) {
        ssize_t n = h->ops.write(h->ops.ctx, h->fd, h->buf + off, w.nbytes - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (off) h->flush_ports = (uint8_t)ports; /* partial waveform is in the ring */
            return -HP_E_IO;
        }
        off += (size_t)n; /* short write: keep going, never truncate the waveform */
    }
    if ((rc = ioc(h, HP_I2S_START, (unsigned long)((uint32_t)ports << 16))) < 0) {
        h->flush_ports = (uint8_t)ports; /* all bytes queued, not started */
        return rc;
    }
    h->pending = true;
    return 0; /* DRAIN is deferred to the next send / hp_hw_sinks_close, so the loop never blocks */
}

/* A key for a Bluetooth host goes to the link (hub/bt_link.h). Best effort by contract: a helper that is down, a host
 * that is not bonded or a full socket loses the key silently, so an activity's IR steps are never aborted by Bluetooth. */
static int bt_key(void *self, uint8_t host, uint8_t page, uint16_t usage, bool down) {
    (void)self;
    if (!bt_link_available()) return -HP_E_NOSYS;
    (void)bt_link_key(host, page, usage, down ? BT_KEY_DOWN : BT_KEY_UP);
    return 0;
}
static int rf_send(void *self, const uint8_t *f, size_t n) { (void)self; (void)f; (void)n; return -HP_E_NOSYS; }
static int rf_recv(void *self, uint8_t *f, size_t cap, int t) { (void)self; (void)f; (void)cap; (void)t; return -HP_E_NOSYS; }
static int rf_claim(void *self) {
    hp_hw_t *h = self;
    if (h->rf_claimed) return -HP_E_BUSY;
    h->rf_claimed = true;
    return 0;
}

int hp_hw_sinks_open(const hp_hw_cfg_t *cfg, hp_hw_t *h) {
    if (!cfg || !h) return -HP_E_INVAL;
    memset(h, 0, sizeof *h);
    h->cfg = *cfg;
    if (!h->cfg.ir_ports) h->cfg.ir_ports = HP_HW_DEFAULT_IR_PORTS;
    h->ops = cfg->ops ? *cfg->ops : REAL_OPS;
    h->fd = -1;
    h->ir = (hp_ir_sink){h, ir_send};
    h->bt = (hp_bt_sink){h, bt_key};
    h->rf = (hp_rf_transport){h, rf_send, rf_recv, rf_claim, NULL, NULL, NULL};
    h->sinks = (hp_sinks_t){&h->ir, &h->bt, &h->rf};
    int fd = open_dev(h);
    if (fd < 0) return fd;
    (void)h->ops.close(h->ops.ctx, fd);
    if (cfg->rf) {
        int rc = hp_rf_spi_open(cfg->rf, &h->rfspi);
        if (rc < 0) return rc;
        h->sinks.rf = h->rfspi;
    }
    return 0;
}

void hp_hw_sinks_close(hp_hw_t *h) {
    if (!h) return;
    if (h->fd >= 0) drain_prev(h);
    h->fd = -1; /* deliberately not closed, see drain_prev */
    hp_rf_spi_destroy(h->rfspi);
    h->rfspi = NULL;
}
