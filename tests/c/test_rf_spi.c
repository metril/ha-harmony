/* hp_rf_transport over /dev/rfspi (hub/io/rf_spi.c) against a fake device behind the syscall seam. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/io/rf_spi.h"
#include "hub/rf/link.h"

#define NW 64
#define NRX 32

typedef struct fake {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int hal;       /* hal_alive result */
    int no_hal_fn; /* seam has no hal_alive */
    int open_errno;
    char opens[8][64];
    int open_flags[8];
    int nopens, nclosed;
    uint8_t w[NW][40];
    size_t wlen[NW];
    int nw;
    int write_mode; /* 0: return 0, 1: return 30, -1: fail with EIO */
    uint8_t rx[NRX][64];
    size_t rxlen[NRX];
    int rxh, rxn;
    int intr;
    int in_read;
    size_t min_cap;
    _Atomic uint64_t now;
    /* in-memory radio bridge (two-endpoint test) */
    struct fake *peer;
    int is_remote;  /* this fake is the remote's device */
    int drop_acks;  /* swallow this many outgoing ack frames */
} fake_t;

static fake_t *F;
struct fake;
static void feed(struct fake *f, const uint8_t *b, size_t n);

static int f_open(void *c, const char *p, int fl) {
    fake_t *f = c;
    pthread_mutex_lock(&f->mu);
    if (f->nopens < 8) {
        snprintf(f->opens[f->nopens], 64, "%s", p);
        f->open_flags[f->nopens] = fl;
    }
    f->nopens++;
    pthread_mutex_unlock(&f->mu);
    if (f->open_errno) {
        errno = f->open_errno;
        return -1;
    }
    return 42;
}

static ssize_t f_write(void *c, int fd, const void *b, size_t n) {
    fake_t *f = c;
    (void)fd;
    pthread_mutex_lock(&f->mu);
    if (f->nw < NW) {
        memcpy(f->w[f->nw], b, n);
        f->wlen[f->nw] = n;
    }
    f->nw++;
    int mode = f->write_mode;
    fake_t *peer = f->peer;
    int drop = 0;
    if (peer && n >= 30 && (((const uint8_t *)b)[0] == 0x12 || ((const uint8_t *)b)[0] == 0x20)) {
        const uint8_t *inner = (const uint8_t *)b + (f->is_remote ? 4 : 2);
        if ((inner[0] & 0xE0) == 0x40 && f->drop_acks > 0) {
            f->drop_acks--;
            drop = 1;
        }
    }
    pthread_mutex_unlock(&f->mu);
    if (peer && !drop && n >= 30) { /* the radios: 30-byte hub frame <-> 32-byte remote frame */
        const uint8_t *p = b;
        uint8_t o[32];
        size_t on;
        if (f->is_remote && n == 32 && p[0] == 0x20) {
            o[0] = 0x12, o[1] = 1; /* the remote is slot 1 on the hub */
            memcpy(o + 2, p + 4, 28);
            on = 30;
            feed(peer, o, on);
        } else if (!f->is_remote && n == 30 && p[0] == 0x12) {
            o[0] = 0x20, o[1] = 0, o[2] = 0x12, o[3] = 0; /* the hub is slot 0 on the remote */
            memcpy(o + 4, p + 2, 28);
            on = 32;
            feed(peer, o, on);
        }
    }
    if (mode < 0) {
        errno = EIO;
        return -1;
    }
    return mode ? 30 : 0;
}

static ssize_t f_read(void *c, int fd, void *b, size_t cap) {
    fake_t *f = c;
    (void)fd;
    pthread_mutex_lock(&f->mu);
    if (f->min_cap == 0 || cap < f->min_cap) f->min_cap = cap;
    f->in_read = 1;
    while (!f->rxn && !f->intr) pthread_cond_wait(&f->cv, &f->mu);
    f->in_read = 0;
    ssize_t r;
    if (f->rxn) {
        r = (ssize_t)f->rxlen[f->rxh];
        memcpy(b, f->rx[f->rxh], f->rxlen[f->rxh]);
        f->rxh = (f->rxh + 1) % NRX;
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

static void fake_init(fake_t *f) {
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->now = 100000;
    F = f;
}

static hp_rf_spi_ops_t fake_ops(fake_t *f) {
    hp_rf_spi_ops_t o = {f, f_open, f_write, f_read, f_close, f->no_hal_fn ? NULL : f_hal, f_now, f_interrupt};
    return o;
}

static int open_fake(fake_t *f, uint8_t slot, hp_rf_transport **t) {
    hp_rf_spi_ops_t o = fake_ops(f);
    hp_rf_spi_cfg_t cfg = {NULL, slot, &o, HP_RF_SPI_ROLE_HUB, 0, false, NULL, NULL};
    return hp_rf_spi_open(&cfg, t);
}

static int open_remote(fake_t *f, uint8_t slot, uint8_t powermode, hp_rf_transport **t) {
    hp_rf_spi_ops_t o = fake_ops(f);
    hp_rf_spi_cfg_t cfg = {NULL, slot, &o, HP_RF_SPI_ROLE_REMOTE, powermode, false, NULL, NULL};
    return hp_rf_spi_open(&cfg, t);
}

static void feed(fake_t *f, const uint8_t *b, size_t n) {
    pthread_mutex_lock(&f->mu);
    memcpy(f->rx[(f->rxh + f->rxn) % NRX], b, n);
    f->rxlen[(f->rxh + f->rxn) % NRX] = n;
    f->rxn++;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

static int nwrites(fake_t *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->nw;
    pthread_mutex_unlock(&f->mu);
    return n;
}

static void ms_sleep(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void wait_attached(hp_rf_transport *t, bool want) {
    for (int i = 0; i < 2000 && t->attached(t->self) != want; i++) ms_sleep(1);
    CT_EQ(t->attached(t->self), want);
}

static void wait_read_blocked(fake_t *f) {
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&f->mu);
        int b = f->in_read;
        pthread_mutex_unlock(&f->mu);
        if (b) return;
        ms_sleep(1);
    }
    CT_CHECK(0 && "reader never blocked in read");
}

/* A 30-byte hub report 0x12, slot, inner[28] with a recognisable inner block. */
static void data_report(uint8_t out[30], uint8_t slot, uint8_t seed) {
    out[0] = 0x12;
    out[1] = slot;
    for (int i = 0; i < 28; i++) out[2 + i] = (uint8_t)(seed + i);
}

/* ---- tests ---- */

static void test_preflight_fails_closed(void) {
    for (int mode = 0; mode < 3; mode++) {
        fake_t f;
        fake_init(&f);
        f.hal = mode == 0 ? 1 : -1;     /* alive / indeterminate */
        f.no_hal_fn = mode == 2;        /* no probe function at all */
        hp_rf_transport *t = NULL;
        CT_EQ(open_fake(&f, 0, &t), -HP_E_HALBUSY);
        CT_CHECK(t == NULL);
        CT_EQ(f.nopens, 0); /* never even opened */
        CT_EQ(f.nw, 0);
    }
    fake_t f; /* open errors map like hw_sinks */
    hp_rf_transport *t = NULL;
    fake_init(&f);
    f.open_errno = EBUSY;
    CT_EQ(open_fake(&f, 0, &t), -HP_E_HALBUSY);
    f.open_errno = ENOENT;
    CT_EQ(open_fake(&f, 0, &t), -HP_E_NOENT);
    f.open_errno = EIO;
    CT_EQ(open_fake(&f, 0, &t), -HP_E_IO);
}

static void test_open_init_and_contract(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[40], in[28] = {0xAA};
    CT_EQ(open_fake(&f, 0, &t), 0);
    CT_EQ(f.nopens, 1);
    CT_CHECK(!strcmp(f.opens[0], "/dev/rfspi"));
    CT_EQ(f.open_flags[0] & O_ACCMODE, O_RDWR);
    CT_EQ(f.nw, 1); /* the init command, once */
    const uint8_t init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
    CT_EQ(f.wlen[0], 7);
    CT_MEM_EQ(f.w[0], init, 7);
    CT_EQ(t->send(t->self, in, sizeof in), -HP_E_INVAL); /* before claim */
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), -HP_E_INVAL);
    CT_EQ(t->claim(t->self), 0);
    CT_EQ(t->claim(t->self), -HP_E_BUSY);
    CT_EQ(t->send(t->self, fr, 29), -HP_E_TOOBIG);
    CT_EQ(t->send(t->self, fr, 30), -HP_E_TOOBIG);
    CT_EQ(t->send(t->self, in, 0), -HP_E_INVAL);
    CT_EQ(f.nw, 1);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), 0);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 20), 0); /* timeout */
    CT_CHECK(!t->attached(t->self));
    CT_CHECK(f.min_cap >= 128); /* the driver ignores count: never pass less than 128 */
    hp_rf_spi_destroy(t);
    CT_EQ(f.nclosed, 1);
    CT_EQ(f.nw, 1);
}

static void test_send_wraps_hub_frame(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t in[28];
    for (int i = 0; i < 28; i++) in[i] = (uint8_t)(0x30 + i);
    CT_EQ(open_fake(&f, 3, &t), 0); /* slot is a parameter */
    CT_EQ(t->claim(t->self), 0);
    CT_EQ(t->send(t->self, in, 28), 0);
    CT_EQ(f.nw, 2);
    CT_EQ(f.wlen[1], 30);
    CT_EQ(f.w[1][0], 0x12);
    CT_EQ(f.w[1][1], 3);
    CT_MEM_EQ(f.w[1] + 2, in, 28);
    CT_EQ(t->send(t->self, in, 5), 0); /* short: zero padded to the fixed frame */
    CT_EQ(f.wlen[2], 30);
    CT_MEM_EQ(f.w[2] + 2, in, 5);
    for (int i = 7; i < 30; i++) CT_EQ(f.w[2][i], 0);
    hp_rf_spi_destroy(t);
}

static void test_write_results_and_backoff(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t in[28] = {1};
    CT_EQ(open_fake(&f, 0, &t), 0);
    t->claim(t->self);
    f.write_mode = 0; /* the normal measured return */
    CT_EQ(t->send(t->self, in, 28), 0);
    f.write_mode = 1; /* positive = an rx length, never a short write */
    CT_EQ(t->send(t->self, in, 28), 0);
    CT_EQ(f.nw, 3);
    f.write_mode = -1;
    const unsigned want[] = {50, 100, 200, 400, 800, 1000, 1000};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        int before = f.nw;
        CT_EQ(t->send(t->self, in, 28), -HP_E_IO);
        CT_EQ(f.nw, before + 1);
        f.now += want[i] - 1; /* still inside the window: suppressed, no syscall */
        CT_EQ(t->send(t->self, in, 28), -HP_E_BUSY);
        CT_EQ(f.nw, before + 1);
        f.now += 1; /* window over: the next send really writes again */
    }
    f.write_mode = 0; /* recovery clears the backoff */
    CT_EQ(t->send(t->self, in, 28), 0);
    CT_EQ(t->send(t->self, in, 28), 0);
    f.write_mode = -1;
    CT_EQ(t->send(t->self, in, 28), -HP_E_IO);
    f.now += 49;
    CT_EQ(t->send(t->self, in, 28), -HP_E_BUSY); /* restarted at 50 ms, not 1 s */
    f.now += 1;
    int n = f.nw;
    CT_EQ(t->send(t->self, in, 28), -HP_E_IO);
    CT_EQ(f.nw, n + 1);
    hp_rf_spi_destroy(t);
}

/* Hub role: data from another slot is not the peer's (it must not release our frame); frame_times stamps
 * arrival (reader clock) and the last write. */
static void test_hub_data_slot_filter_and_stamps(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[64], rep[30];
    CT_EQ(open_fake(&f, 2, &t), 0);
    t->claim(t->self);
    CT_CHECK(t->frame_times != NULL);
    f.now = 100100;
    data_report(rep, 1, 0x10); /* another slot */
    feed(&f, rep, 30);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 100), 0);
    f.now = 100200;
    data_report(rep, 2, 0x20);
    feed(&f, rep, 30);
    wait_attached(t, true); /* queued by the reader at 100200 */
    f.now = 100300;         /* read later than it arrived */
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    CT_MEM_EQ(fr, rep + 2, 28);
    uint32_t rx = 0, tx = 0;
    CT_EQ(t->frame_times(t->self, &rx, &tx), 1);
    CT_EQ(rx, 100200u); /* the reader's arrival time, not the read time */
    f.now = 100400;
    CT_EQ(t->send(t->self, fr, 28), 0);
    CT_EQ(t->frame_times(t->self, &rx, &tx), 1);
    CT_EQ(tx, 100400u);
    hp_rf_spi_destroy(t);
}

static void test_reader_classifies(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[64], rep[30];
    CT_EQ(open_fake(&f, 0, &t), 0);
    t->claim(t->self);

    /* 0x12 data: the inner block comes out, the 2-byte header does not */
    data_report(rep, 1, 0x40);
    feed(&f, rep, 30);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    CT_MEM_EQ(fr, rep + 2, 28);
    CT_CHECK(t->attached(t->self));

    /* garbage and unknown control reports: dropped, never surfaced */
    const uint8_t junk1[3] = {0xFF, 0x01, 0x02};
    uint8_t junk2[64];
    memset(junk2, 0x55, sizeof junk2);
    const uint8_t ctl[7] = {0x10, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00}; /* TX status, not link status */
    uint8_t short12[29];
    memset(short12, 0x12, sizeof short12); /* 0x12 but not 30 bytes */
    feed(&f, junk1, sizeof junk1);
    feed(&f, junk2, sizeof junk2);
    feed(&f, ctl, sizeof ctl);
    feed(&f, short12, sizeof short12);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 100), 0);
    CT_CHECK(t->attached(t->self)); /* none of them touched the link state */

    /* frames queued before a loss come first, then NOENT exactly once */
    const uint8_t lost[7] = {0x10, 0x01, 0x41, 0x04, 0x40, 0x00, 0x00};
    data_report(rep, 1, 0x60);
    feed(&f, rep, 30);
    feed(&f, lost, sizeof lost);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    CT_MEM_EQ(fr, rep + 2, 28);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), -HP_E_NOENT);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), 0);
    CT_CHECK(!t->attached(t->self));
    feed(&f, lost, sizeof lost); /* already lost: not a second transition */
    ms_sleep(30);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 20), 0);

    /* link up (this slot), other slots ignored */
    const uint8_t other[7] = {0x10, 0x02, 0x41, 0x04, 0x00, 0x00, 0x00};
    feed(&f, other, sizeof other);
    ms_sleep(30);
    CT_CHECK(!t->attached(t->self));
    const uint8_t up[7] = {0x10, 0x01, 0x41, 0x04, 0x01, 0x00, 0x00}; /* bit 0x40 clear */
    feed(&f, up, sizeof up);
    wait_attached(t, true);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 20), 0); /* link-up is not a frame */
    const uint8_t lost2[7] = {0x10, 0x01, 0x41, 0x04, 0x4F, 0x00, 0x00};
    feed(&f, lost2, sizeof lost2); /* any status with 0x40 set */
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), -HP_E_NOENT);
    CT_CHECK(!t->attached(t->self));

    /* a small recv buffer drops the frame instead of wedging the queue */
    data_report(rep, 1, 0x10);
    feed(&f, rep, 30);
    CT_EQ(t->recv(t->self, fr, 10, 2000), -HP_E_TOOBIG);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), 0);
    hp_rf_spi_destroy(t);
}

static void test_wake_fd(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[64], rep[30];
    CT_EQ(open_fake(&f, 0, &t), 0);
    t->claim(t->self);
    int fd = hp_rf_spi_wake_fd(t);
    CT_CHECK(fd >= 0);
    struct pollfd p = {fd, POLLIN, 0};
    CT_EQ(poll(&p, 1, 0), 0);
    data_report(rep, 1, 1);
    feed(&f, rep, 30);
    p.events = POLLIN;
    CT_EQ(poll(&p, 1, 2000), 1);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), 28);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 0), 0);
    p.revents = 0;
    CT_EQ(poll(&p, 1, 0), 0); /* drained once nothing is left to return */
    CT_EQ(hp_rf_spi_wake_fd(NULL), -1);
    hp_rf_spi_destroy(t);
}

static void test_destroy_wakes_blocked_reader(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    CT_EQ(open_fake(&f, 0, &t), 0);
    wait_read_blocked(&f); /* the fake's read() blocks until interrupted */
    alarm(20);             /* a hang here is a failure, not a stuck CI job */
    hp_rf_spi_destroy(t);  /* returns only if the reader was woken and joined */
    alarm(0);
    CT_EQ(f.nclosed, 1);
}

/* The real mechanism: a pipe read() that only a SIGUSR1 (no SA_RESTART) can interrupt. */
static int g_pipe[2];
static int p_open(void *c, const char *p, int fl) { (void)c; (void)p; (void)fl; return g_pipe[0]; }
static ssize_t p_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static ssize_t p_write(void *c, int fd, const void *b, size_t n) { (void)c; (void)fd; (void)b; (void)n; return 0; }
static int p_close(void *c, int fd) { (void)c; (void)fd; return 0; }
static int p_hal(void *c) { (void)c; return 0; }

static void test_destroy_real_signal(void) {
    CT_EQ(pipe(g_pipe), 0);
    hp_rf_spi_ops_t o = hp_rf_spi_real_ops(); /* real interrupt = pthread_kill(SIGUSR1) */
    o.open = p_open;
    o.read = p_read;
    o.write = p_write;
    o.close = p_close;
    o.hal_alive = p_hal;
    hp_rf_spi_cfg_t cfg = {NULL, 0, &o, HP_RF_SPI_ROLE_HUB, 0, false, NULL, NULL};
    hp_rf_transport *t = NULL;
    CT_EQ(hp_rf_spi_open(&cfg, &t), 0);
    ms_sleep(50); /* let the reader block in read() */
    alarm(20);
    hp_rf_spi_destroy(t);
    alarm(0);
    close(g_pipe[0]);
    close(g_pipe[1]);
}

static void test_real_hal_check_is_decisive(void) {
    hp_rf_spi_ops_t o = hp_rf_spi_real_ops();
    int r = o.hal_alive(NULL); /* walks /proc: on a Linux host it can tell (and no hal runs here) */
    CT_CHECK(r == 0 || r == 1);
}

static void test_brick_safety_and_explicit_pairing(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[64], rep[30], in[28] = {7};
    CT_EQ(open_fake(&f, 0, &t), 0);
    t->claim(t->self);
    /* a full session: traffic both ways, errors, link loss */
    data_report(rep, 1, 9);
    feed(&f, rep, 30);
    t->recv(t->self, fr, sizeof fr, 2000);
    t->send(t->self, in, 28);
    f.write_mode = -1;
    t->send(t->self, in, 28);
    f.write_mode = 0;
    const uint8_t lost[7] = {0x10, 0x01, 0x41, 0x04, 0x40, 0x00, 0x00};
    feed(&f, lost, sizeof lost);
    t->recv(t->self, fr, sizeof fr, 2000);

    CT_EQ(f.nopens, 1); /* only /dev/rfspi: never /dev/rffw, never anything else */
    CT_CHECK(!strcmp(f.opens[0], "/dev/rfspi"));
    for (int i = 0; i < f.nw; i++) {
        CT_CHECK(!(f.wlen[i] == 7 && f.w[i][0] == 0x10 && f.w[i][2] == 0x80 && f.w[i][3] == 0xB2)); /* no pair/unpair */
        CT_CHECK(!(f.wlen[i] == 7 && f.w[i][0] == 0x20));                                            /* no power mode */
        CT_CHECK(f.wlen[i] == 7 || f.wlen[i] == 30);
    }

    f.now += 5000; /* explicit API only */
    int n = f.nw;
    CT_EQ(hp_rf_spi_set_pairing(t, 0), -HP_E_INVAL);
    CT_EQ(hp_rf_spi_set_pairing(t, 256), -HP_E_INVAL);
    CT_EQ(hp_rf_spi_unpair(t, 0), -HP_E_INVAL);
    CT_EQ(hp_rf_spi_unpair(t, 16), -HP_E_INVAL);
    CT_EQ(hp_rf_spi_set_pairing(NULL, 5), -HP_E_INVAL);
    CT_EQ(f.nw, n);
    CT_EQ(hp_rf_spi_set_pairing(t, 60), 0);
    const uint8_t pair[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x00, 60};
    CT_EQ(f.wlen[n], 7);
    CT_MEM_EQ(f.w[n], pair, 7);
    CT_EQ(hp_rf_spi_unpair(t, 1), 0);
    const uint8_t unp[7] = {0x10, 0xFF, 0x80, 0xB2, 0x03, 0x01, 0x00};
    CT_EQ(f.wlen[n + 1], 7);
    CT_MEM_EQ(f.w[n + 1], unp, 7);
    CT_EQ(f.nw, n + 2);
    hp_rf_spi_destroy(t);
}

/* ---- remote role ---- */

/* A 32-byte remote report 0x20, byte1, 0x12, slot, inner[28]. */
static void remote_report(uint8_t out[32], uint8_t slot, uint8_t seed) {
    out[0] = 0x20;
    out[1] = slot;
    out[2] = 0x12;
    out[3] = slot;
    for (int i = 0; i < 28; i++) out[4 + i] = (uint8_t)(seed + i);
}

static void test_remote_wrap_and_cap(void) {
    uint8_t in[28];
    for (int i = 0; i < 28; i++) in[i] = (uint8_t)(0x50 + i);
    const struct { uint8_t slot, pm, want1; } cases[] = {
        {0, 0x00, 0x00}, {1, 0x00, 0x01}, {0, 0x30, 0x30}, {5, 0x40, 0x45}, {0x12, 0x10, 0x12}, {0, 0x0F, 0x00}};
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        fake_t f;
        fake_init(&f);
        hp_rf_transport *t = NULL;
        CT_EQ(open_remote(&f, cases[k].slot, cases[k].pm, &t), 0);
        const uint8_t init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
        CT_EQ(f.wlen[0], 7); /* init is sent in the remote role too */
        CT_MEM_EQ(f.w[0], init, 7);
        CT_EQ(t->claim(t->self), 0);
        CT_EQ(t->send(t->self, in, 28), 0);
        CT_EQ(f.wlen[1], 32);
        CT_EQ(f.w[1][0], 0x20);
        CT_EQ(f.w[1][1], cases[k].want1);
        CT_EQ(f.w[1][2], 0x12);
        CT_EQ(f.w[1][3], cases[k].slot); /* default slot 0 is the hub's pipe */
        CT_MEM_EQ(f.w[1] + 4, in, 28);
        CT_EQ(t->send(t->self, in, 3), 0); /* short: zero padded to 32 */
        CT_EQ(f.wlen[2], 32);
        for (int i = 7; i < 32; i++) CT_EQ(f.w[2][i], 0);
        CT_EQ(t->send(t->self, in, 29), -HP_E_TOOBIG);
        hp_rf_spi_destroy(t);
    }

    /* the 32-byte cap: nothing longer ever reaches the syscall, in either direction of the guard */
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t big[64] = {0x20};
    CT_EQ(open_remote(&f, 0, 0, &t), 0);
    int n = f.nw;
    CT_EQ(hp_rf_spi_write_for_test(t, big, 33), -HP_E_TOOBIG);
    CT_EQ(hp_rf_spi_write_for_test(t, big, 64), -HP_E_TOOBIG);
    CT_EQ(f.nw, n);
    CT_EQ(hp_rf_spi_write_for_test(t, big, 32), 0); /* exactly the cap is fine */
    CT_EQ(f.nw, n + 1);
    CT_EQ(f.wlen[n], 32);
    hp_rf_spi_destroy(t);
    CT_EQ(HP_RF_SPI_REMOTE_WRITE_CAP, 32);
}

static void test_role_classification(void) {
    uint8_t fr[64], hub30[30], rem32[32];
    data_report(hub30, 1, 0x40);
    remote_report(rem32, 0, 0x70);

    /* remote role: 0x20 data in, 0x12 and malformed 0x20 dropped */
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    CT_EQ(open_remote(&f, 0, 0, &t), 0);
    t->claim(t->self);
    feed(&f, hub30, 30);
    uint8_t bad[32];
    memcpy(bad, rem32, 32);
    bad[2] = 0x13; /* wrong inner marker */
    feed(&f, bad, 32);
    feed(&f, rem32, 31); /* wrong length */
    CT_EQ(t->recv(t->self, fr, sizeof fr, 100), 0);
    CT_CHECK(!t->attached(t->self));
    feed(&f, rem32, 32);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    CT_MEM_EQ(fr, rem32 + 4, 28);
    CT_CHECK(t->attached(t->self)); /* the first data frame attaches */

    /* link status in the remote role: this slot's loss, queued frames first, then NOENT once */
    const uint8_t lost[7] = {0x10, 0x00, 0x41, 0x04, 0x40, 0x00, 0x00};
    const uint8_t other[7] = {0x10, 0x01, 0x41, 0x04, 0x40, 0x00, 0x00};
    feed(&f, other, sizeof other);
    ms_sleep(30);
    CT_CHECK(t->attached(t->self));
    feed(&f, rem32, 32);
    feed(&f, lost, sizeof lost);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), -HP_E_NOENT);
    CT_CHECK(!t->attached(t->self));
    const uint8_t up[7] = {0x10, 0x00, 0x41, 0x04, 0x01, 0x00, 0x00};
    feed(&f, up, sizeof up);
    wait_attached(t, true);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 20), 0);
    hp_rf_spi_destroy(t);

    /* hub role: 0x12 in, a 0x20 report is dropped */
    fake_init(&f);
    CT_EQ(open_fake(&f, 0, &t), 0);
    t->claim(t->self);
    feed(&f, rem32, 32);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 100), 0);
    CT_CHECK(!t->attached(t->self));
    feed(&f, hub30, 30);
    CT_EQ(t->recv(t->self, fr, sizeof fr, 2000), 28);
    hp_rf_spi_destroy(t);
}

static void test_remote_pairing_refused_and_brick_safety(void) {
    fake_t f;
    fake_init(&f);
    hp_rf_transport *t = NULL;
    uint8_t fr[64], in[28] = {7};
    CT_EQ(open_remote(&f, 0, 0x30, &t), 0);
    t->claim(t->self);
    uint8_t rep[32];
    remote_report(rep, 0, 9);
    feed(&f, rep, 32);
    t->recv(t->self, fr, sizeof fr, 2000);
    t->send(t->self, in, 28);
    f.write_mode = -1;
    t->send(t->self, in, 28);
    f.write_mode = 0;
    f.now += 5000;
    int n = f.nw;
    CT_EQ(hp_rf_spi_set_pairing(t, 60), -HP_E_INVAL);
    CT_EQ(hp_rf_spi_unpair(t, 1), -HP_E_INVAL);
    CT_EQ(f.nw, n); /* nothing written */
    CT_EQ(f.nopens, 1); /* only /dev/rfspi: never /dev/rffw */
    CT_CHECK(!strcmp(f.opens[0], "/dev/rfspi"));
    for (int i = 0; i < f.nw && i < NW; i++) {
        CT_CHECK(f.wlen[i] == 7 || f.wlen[i] == 32);
        CT_CHECK(!(f.wlen[i] == 7 && f.w[i][3] == 0xB2)); /* no pair/unpair */
        CT_CHECK(f.wlen[i] <= 32);
    }
    hp_rf_spi_destroy(t);
    CT_EQ(f.nclosed, 1);
}

/* ---- two real hp_rf_link_t endpoints over the radio bridge ---- */

typedef struct {
    int n[32]; /* deliveries per message type */
    uint8_t last[32][8];
} sink_t;

static void sink(void *ctx, const hp_rf_msg_t *m) {
    sink_t *k = ctx;
    if (m->type < 32) {
        k->n[m->type]++;
        memcpy(k->last[m->type], m->payload, m->len < 8 ? m->len : 8);
    }
}

static void run_pumps(hp_rf_link_t *h, hp_rf_link_t *r, fake_t *fh, int steps) {
    for (int i = 0; i < steps; i++) {
        ms_sleep(3); /* let the reader threads move bytes */
        fh->now += 50;
        uint32_t now = (uint32_t)fh->now;
        hp_rf_link_pump(h, now);
        hp_rf_link_pump(r, now);
    }
}

static void test_two_endpoint_link(void) {
    for (int dropped = 0; dropped < 3; dropped++) {
        fake_t fh, fr;
        fake_init(&fh);
        fake_init(&fr);
        fr.is_remote = 1;
        fh.peer = &fr;
        fr.peer = &fh;
        hp_rf_transport *th = NULL, *tr = NULL;
        CT_EQ(open_fake(&fh, 0, &th), 0);
        CT_EQ(open_remote(&fr, 0, 0, &tr), 0);
        th->claim(th->self);
        tr->claim(tr->self);
        const uint8_t hup[7] = {0x10, 0x01, 0x41, 0x04, 0x01, 0, 0}; /* remote is slot 1 on the hub */
        const uint8_t rup[7] = {0x10, 0x00, 0x41, 0x04, 0x01, 0, 0}; /* hub is slot 0 on the remote */
        feed(&fh, hup, 7);
        feed(&fr, rup, 7);
        wait_attached(th, true);
        wait_attached(tr, true);

        sink_t hs, rs;
        memset(&hs, 0, sizeof hs);
        memset(&rs, 0, sizeof rs);
        hp_rf_link_t hl, rl;
        hp_rf_link_init(&hl, th, false, 1, sink, &hs);
        hp_rf_link_init(&rl, tr, false, 0, sink, &rs);

        fr.drop_acks = dropped; /* the remote's ack to a page_push is lost: the hub retransmits */
        fh.drop_acks = dropped; /* the hub's ack to a key_event is lost: the remote retransmits */
        const uint8_t key[3] = {0x11, 0x22, 0x33};
        const uint8_t page[40] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                  21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
        CT_EQ(hp_rf_link_send(&rl, HP_MSG_KEY_EVENT, key, sizeof key, (uint32_t)fh.now), 0);
        CT_EQ(hp_rf_link_send(&hl, HP_MSG_PAGE_PUSH, page, sizeof page, (uint32_t)fh.now), 0);
        run_pumps(&hl, &rl, &fh, 40);

        CT_EQ(hs.n[HP_MSG_KEY_EVENT], 1); /* exactly once, despite retransmits */
        CT_MEM_EQ(hs.last[HP_MSG_KEY_EVENT], key, 3);
        CT_EQ(rs.n[HP_MSG_PAGE_PUSH], 1);
        CT_MEM_EQ(rs.last[HP_MSG_PAGE_PUSH], page, 8);
        hp_rf_link_status_t st = hp_rf_link_status(&rl);
        CT_EQ(st.queued, 0);
        CT_EQ(st.failures, 0);
        CT_EQ(hp_rf_link_status(&hl).failures, 0);
        /* every remote write was a legal 32-byte frame (or the init command) */
        for (int i = 0; i < fr.nw && i < NW; i++) CT_CHECK(fr.wlen[i] == 7 || fr.wlen[i] == 32);
        for (int i = 0; i < fh.nw && i < NW; i++) CT_CHECK(fh.wlen[i] == 7 || fh.wlen[i] == 30);
        hp_rf_spi_destroy(th);
        hp_rf_spi_destroy(tr);
    }
}

/* ---- diagnostics, TX status, link-status logs ---- */

typedef struct {
    pthread_mutex_t mu;
    char line[64][256];
    int n;
} cap_t;

static void cap_init(cap_t *c) {
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
}

static void cap_log(void *ctx, const char *l) {
    cap_t *c = ctx;
    pthread_mutex_lock(&c->mu);
    if (c->n < 64) snprintf(c->line[c->n], sizeof c->line[0], "%s", l);
    c->n++;
    pthread_mutex_unlock(&c->mu);
}

static int cap_count(cap_t *c, const char *needle) {
    int k = 0;
    pthread_mutex_lock(&c->mu);
    for (int i = 0; i < c->n && i < 64; i++)
        if (strstr(c->line[i], needle)) k++;
    pthread_mutex_unlock(&c->mu);
    return k;
}

static int cap_total(cap_t *c) {
    pthread_mutex_lock(&c->mu);
    int n = c->n;
    pthread_mutex_unlock(&c->mu);
    return n;
}

static int open_cfg(fake_t *f, hp_rf_spi_role_t role, uint8_t slot, bool debug, cap_t *c, hp_rf_transport **t) {
    hp_rf_spi_ops_t o = fake_ops(f);
    hp_rf_spi_cfg_t cfg = {NULL, slot, &o, role, 0, debug, cap_log, c};
    return hp_rf_spi_open(&cfg, t);
}

static bool wait_txs(hp_rf_transport *t, uint8_t *slot, uint8_t *st) {
    for (int i = 0; i < 2000; i++) {
        if (t->tx_status(t->self, slot, st)) return true;
        ms_sleep(1);
    }
    return false;
}

static void test_tx_status_surfaced(void) {
    fake_t f;
    cap_t c;
    fake_init(&f);
    cap_init(&c);
    hp_rf_transport *t = NULL;
    CT_EQ(open_cfg(&f, HP_RF_SPI_ROLE_HUB, 2, false, &c, &t), 0);
    t->claim(t->self);
    CT_CHECK(t->tx_status != NULL);
    uint8_t r0[32] = {0x20, 0x02, 0x42, 0x00}, r1[32] = {0x20, 0x02, 0x42, 0x01}, other[32] = {0x20, 0x03, 0x42, 0x00};
    uint8_t slot = 0xEE, st = 0xEE;
    CT_EQ(t->tx_status(t->self, &slot, &st), 0); /* none pending */
    feed(&f, r0, 32);
    feed(&f, other, 32); /* another slot: not ours */
    feed(&f, r1, 32);
    CT_CHECK(wait_txs(t, &slot, &st));
    CT_EQ(slot, 2);
    CT_EQ(st, 0);
    CT_CHECK(wait_txs(t, &slot, &st));
    CT_EQ(slot, 2);
    CT_EQ(st, 1);
    ms_sleep(20);
    CT_EQ(t->tx_status(t->self, &slot, &st), 0);
    uint8_t buf[HP_RF_INNER_LEN];
    CT_EQ(t->recv(t->self, buf, sizeof buf, 0), 0); /* never queued as data */
    CT_CHECK(!t->attached(t->self));                /* and not a link-up either */
    hp_rf_spi_destroy(t);

    /* remote role: the same 0x20/0x42 shape, any slot, byte1 & 0x0F */
    fake_t g;
    fake_init(&g);
    hp_rf_transport *r = NULL;
    CT_EQ(open_cfg(&g, HP_RF_SPI_ROLE_REMOTE, 0, false, &c, &r), 0);
    r->claim(r->self);
    uint8_t rs[32] = {0x20, 0x31, 0x42, 0x01};
    feed(&g, rs, 32);
    CT_CHECK(wait_txs(r, &slot, &st));
    CT_EQ(slot, 1);
    CT_EQ(st, 1);
    hp_rf_spi_destroy(r);
}

static void test_link_transition_logs(void) {
    fake_t f;
    cap_t c;
    fake_init(&f);
    cap_init(&c);
    hp_rf_transport *t = NULL;
    CT_EQ(open_cfg(&f, HP_RF_SPI_ROLE_HUB, 2, false, &c, &t), 0);
    t->claim(t->self);
    const uint8_t up[7] = {0x10, 0x02, 0x41, 0x04, 0x87, 0x21, 0x40};
    const uint8_t lost[7] = {0x10, 0x02, 0x41, 0x04, 0x47, 0x21, 0x40};
    const uint8_t wrong[7] = {0x10, 0x01, 0x41, 0x04, 0x47, 0x21, 0x40}; /* another slot: ignored */
    feed(&f, up, 7);
    wait_attached(t, true);
    feed(&f, up, 7);
    feed(&f, wrong, 7);
    feed(&f, lost, 7);
    feed(&f, lost, 7);
    wait_attached(t, false);
    feed(&f, up, 7);
    wait_attached(t, true);
    feed(&f, up, 7); /* queued behind: once the reader has it, the log would show */
    ms_sleep(30);
    CT_EQ(cap_count(&c, "link up (slot 2, status 0x87, wpid 4021)"), 2);
    CT_EQ(cap_count(&c, "link lost (status 0x47)"), 1);
    CT_EQ(cap_total(&c), 3); /* exactly one line per transition, no debug lines */
    hp_rf_spi_destroy(t);
}

static void test_rf_debug_frames(void) {
    for (int dbg = 0; dbg < 2; dbg++) {
        fake_t f;
        cap_t c;
        fake_init(&f);
        cap_init(&c);
        hp_rf_transport *t = NULL;
        CT_EQ(open_cfg(&f, HP_RF_SPI_ROLE_HUB, 2, dbg, &c, &t), 0);
        t->claim(t->self);
        CT_EQ(cap_total(&c), dbg ? 1 : 0); /* the init command write */
        if (dbg) CT_EQ(cap_count(&c, " TX 0: 10 ff 80 00 00 01 00"), 1);
        uint8_t fr[3] = {0xA1, 0xB2, 0xC3};
        CT_EQ(t->send(t->self, fr, sizeof fr), 0);
        CT_EQ(cap_total(&c), dbg ? 2 : 0);
        uint8_t rx[30] = {0x12, 0x02, 0xD4, 0xE5};
        feed(&f, rx, 30);
        uint8_t buf[HP_RF_INNER_LEN];
        CT_EQ(t->recv(t->self, buf, sizeof buf, 1000), HP_RF_INNER_LEN);
        if (dbg) {
            CT_EQ(cap_total(&c), 3);
            CT_EQ(cap_count(&c, " TX 0: 12 02 a1 b2 c3 00 00"), 1);
            CT_EQ(cap_count(&c, " RX 30: 12 02 d4 e5 00 00"), 1);
            CT_EQ(cap_count(&c, " ms TX 0:"), 2);
            pthread_mutex_lock(&c.mu);
            CT_CHECK(strncmp(c.line[1], "rf_spi ", 7) == 0);
            pthread_mutex_unlock(&c.mu);
        } else {
            CT_EQ(cap_total(&c), 0);
        }
        hp_rf_spi_destroy(t);
    }
}

typedef struct {
    int n, last_len;
    uint32_t sum;
} sweep_sink_t;

static void sweep_deliver(void *ctx, const hp_rf_msg_t *m) {
    sweep_sink_t *k = ctx;
    k->n++;
    k->last_len = (int)m->len;
    k->sum = 0;
    for (size_t i = 0; i < m->len; i++) k->sum = k->sum * 31 + m->payload[i];
}

/* Every body size 0..254, alternating hub->remote / remote->hub, over the paced link and both drivers. */
static void test_two_endpoint_every_size(void) {
    fake_t fh, fr;
    fake_init(&fh);
    fake_init(&fr);
    fr.is_remote = 1;
    fh.peer = &fr;
    fr.peer = &fh;
    hp_rf_transport *th = NULL, *tr = NULL;
    CT_EQ(open_fake(&fh, 0, &th), 0);
    CT_EQ(open_remote(&fr, 0, 0, &tr), 0);
    th->claim(th->self);
    tr->claim(tr->self);
    const uint8_t hup[7] = {0x10, 0x01, 0x41, 0x04, 0x01, 0, 0};
    const uint8_t rup[7] = {0x10, 0x00, 0x41, 0x04, 0x01, 0, 0};
    feed(&fh, hup, 7);
    feed(&fr, rup, 7);
    wait_attached(th, true);
    wait_attached(tr, true);
    sweep_sink_t hs = {0}, rs = {0};
    hp_rf_link_t hl, rl;
    hp_rf_link_init(&hl, th, false, 1, sweep_deliver, &hs);
    hp_rf_link_init(&rl, tr, false, 0, sweep_deliver, &rs);
    for (size_t len = 0; len <= HP_RF_BODY_MAX; len++) {
        uint8_t body[HP_RF_BODY_MAX];
        uint32_t want = 0;
        for (size_t i = 0; i < len; i++) {
            body[i] = (uint8_t)(i * 5 + len);
            want = want * 31 + body[i];
        }
        bool to_remote = len % 2 == 0;
        hp_rf_link_t *from = to_remote ? &hl : &rl;
        sweep_sink_t *k = to_remote ? &rs : &hs;
        int before = k->n;
        CT_EQ(hp_rf_link_send(from, HP_MSG_PAGE_PUSH, body, len, (uint32_t)fh.now), 0);
        for (int step = 0; step < 60 && (k->n == before || hp_rf_link_status(from).queued); step++) {
            ms_sleep(1);
            fh.now += 25;
            hp_rf_link_pump(&hl, (uint32_t)fh.now);
            hp_rf_link_pump(&rl, (uint32_t)fh.now);
        }
        CT_EQ(k->n, before + 1); /* exactly once */
        CT_EQ(k->last_len, (int)len);
        CT_EQ(k->sum, want);
        if (k->n != before + 1) break;
    }
    CT_EQ(hp_rf_link_status(&hl).failures, 0);
    CT_EQ(hp_rf_link_status(&rl).failures, 0);
    hp_rf_spi_destroy(th);
    hp_rf_spi_destroy(tr);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL; /* SIGALRM watchdog kills a hung test */
    sigaction(SIGALRM, &sa, NULL);
    CT_RUN(test_preflight_fails_closed);
    CT_RUN(test_open_init_and_contract);
    CT_RUN(test_send_wraps_hub_frame);
    CT_RUN(test_write_results_and_backoff);
    CT_RUN(test_reader_classifies);
    CT_RUN(test_wake_fd);
    CT_RUN(test_destroy_wakes_blocked_reader);
    CT_RUN(test_destroy_real_signal);
    CT_RUN(test_real_hal_check_is_decisive);
    CT_RUN(test_brick_safety_and_explicit_pairing);
    CT_RUN(test_remote_wrap_and_cap);
    CT_RUN(test_role_classification);
    CT_RUN(test_remote_pairing_refused_and_brick_safety);
    CT_RUN(test_two_endpoint_link);
    CT_RUN(test_two_endpoint_every_size);
    CT_RUN(test_tx_status_surfaced);
    CT_RUN(test_hub_data_slot_filter_and_stamps);
    CT_RUN(test_link_transition_logs);
    CT_RUN(test_rf_debug_frames);
    (void)nwrites;
    return CT_DONE();
}
