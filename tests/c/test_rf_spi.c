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

#define NW 64
#define NRX 32

typedef struct {
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
} fake_t;

static fake_t *F;

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
    pthread_mutex_unlock(&f->mu);
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
    hp_rf_spi_cfg_t cfg = {NULL, slot, &o};
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
    hp_rf_spi_cfg_t cfg = {NULL, 0, &o};
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
    (void)nwrites;
    return CT_DONE();
}
