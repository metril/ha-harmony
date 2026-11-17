#define _POSIX_C_SOURCE 200809L
#include "hub/io/rf_spi.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hub/hp_errors.h"
#include "hub/io/hal_check.h"
#include "hub/rf/framing.h"

#define RXQ 16        /* received inner blocks buffered for recv() */
#define READ_BUF 128  /* the driver ignores `count` and copies a 64-byte entry: never pass less */
#define RX_MAX 64
#define LOG_MAX 32    /* diagnostics are rate-limited by total count */
#define REPORT_HUB_DATA 0x12
#define REPORT_REMOTE_DATA 0x20
#define LINK_LOST 0x40
#define TXSQ 8
#define TX_STATUS_ID 0x42

/* The remote's /dev/rfspi memcpys `count` bytes into a 32-byte kernel slab with no check: a longer
 * write overflows kernel heap. spi_write() enforces this cap for every remote-role write. */
#define REMOTE_WRITE_CAP 32
_Static_assert(HP_RF_REMOTE_FRAME_LEN <= REMOTE_WRITE_CAP, "remote frame must fit the kernel slab");
_Static_assert(HP_RF_SPI_REMOTE_WRITE_CAP == REMOTE_WRITE_CAP, "header and driver caps agree");

static const uint8_t CMD_INIT[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};

typedef struct {
    hp_rf_transport t; /* first member */
    hp_rf_spi_ops_t ops;
    uint8_t slot;
    hp_rf_spi_role_t role;
    uint8_t powermode;
    int fd;
    bool claimed;

    pthread_mutex_t mu; /* guards the receive state below */
    pthread_cond_t cv;
    uint8_t q[RXQ][HP_RF_INNER_LEN];
    size_t qhead, qn;
    bool attached, lost_pending;
    int wake[2]; /* self-pipe: readable while recv() has something to return */
    unsigned logged;
    bool reader_done;
    pthread_t reader;
    volatile sig_atomic_t stop;

    pthread_mutex_t tx_mu; /* serialises write() and its backoff state */
    uint64_t next_ok;
    unsigned delay_ms;

    bool debug;
    void (*log)(void *ctx, const char *line);
    void *log_ctx;
    uint64_t t0;
    int link_logged; /* -1 nothing logged yet, 0 lost, 1 up */
    uint8_t txs[TXSQ][2]; /* pending TX statuses: slot, status */
    size_t txs_head, txs_n;
} spi_t;

/* ---- real syscalls ---- */

static int r_open(void *c, const char *p, int f) { (void)c; return open(p, f); }
static ssize_t r_write(void *c, int fd, const void *b, size_t n) { (void)c; return write(fd, b, n); }
static ssize_t r_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static int r_close(void *c, int fd) { (void)c; return close(fd); }
static int r_hal(void *c) { (void)c; return hp_hal_alive_proc(); }
static uint64_t r_now(void *c) {
    (void)c;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void r_interrupt(void *c, pthread_t t) { (void)c; pthread_kill(t, SIGUSR1); }

hp_rf_spi_ops_t hp_rf_spi_real_ops(void) {
    return (hp_rf_spi_ops_t){NULL, r_open, r_write, r_read, r_close, r_hal, r_now, r_interrupt};
}

static void on_usr1(int sig) { (void)sig; } /* only exists to make read() return EINTR */

static void install_usr1(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1; /* deliberately no SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

/* ---- helpers ---- */

static void logf_limited(spi_t *s, const char *what, const uint8_t *b, size_t n) {
    if (s->logged >= LOG_MAX) return;
    s->logged++;
    fprintf(stderr, "rf_spi: %s (%zu bytes):", what, n);
    for (size_t i = 0; i < n && i < 8; i++) fprintf(stderr, " %02x", b[i]);
    fputc('\n', stderr);
}

/* Diagnostics sink (default stderr). */
static void emit_line(spi_t *s, const char *line) {
    if (s->log) s->log(s->log_ctx, line);
    else fprintf(stderr, "%s\n", line);
}

static void slogf(spi_t *s, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit_line(s, line);
}

/* --rf-debug: one line per syscall. `rc` is write()'s return or read()'s length. */
static void dbg_frame(spi_t *s, const char *dir, long rc, const uint8_t *b, size_t n) {
    if (!s->debug) return;
    char line[16 + 24 + 3 * RX_MAX + 8];
    int k = snprintf(line, sizeof line, "rf_spi %8llu ms %s %ld:", (unsigned long long)(s->ops.now_ms(s->ops.ctx) - s->t0), dir, rc);
    for (size_t i = 0; i < n && i < RX_MAX && (size_t)k + 4 < sizeof line; i++) k += snprintf(line + k, sizeof line - (size_t)k, " %02x", b[i]);
    emit_line(s, line);
}

static void wake(spi_t *s) {
    uint8_t b = 1;
    if (write(s->wake[1], &b, 1) < 0) { /* pipe full = already readable */ }
}

/* The only write() site. 0 ok (including a positive return, which is an rx length), -HP_E_BUSY while
 * backing off after an error, -HP_E_IO on a negative return. */
static int spi_write(spi_t *s, const uint8_t *buf, size_t n) {
    if (s->role == HP_RF_SPI_ROLE_REMOTE && n > REMOTE_WRITE_CAP) return -HP_E_TOOBIG; /* never reaches the syscall */
    pthread_mutex_lock(&s->tx_mu);
    uint64_t now = s->ops.now_ms(s->ops.ctx);
    int rc = 0;
    if (s->delay_ms && now < s->next_ok) {
        rc = -HP_E_BUSY;
    } else {
        ssize_t w;
        do w = s->ops.write(s->ops.ctx, s->fd, buf, n);
        while (w < 0 && errno == EINTR);
        dbg_frame(s, "TX", (long)w, buf, n);
        if (w < 0) {
            unsigned d = s->delay_ms ? s->delay_ms * 2 : HP_RF_SPI_BACKOFF_MIN_MS;
            s->delay_ms = d > HP_RF_SPI_BACKOFF_MAX_MS ? HP_RF_SPI_BACKOFF_MAX_MS : d;
            s->next_ok = now + s->delay_ms;
            rc = -HP_E_IO;
        } else {
            s->delay_ms = 0;
        }
    }
    pthread_mutex_unlock(&s->tx_mu);
    return rc;
}

/* ---- reader ---- */

static void push_event(spi_t *s) {
    pthread_cond_broadcast(&s->cv);
    wake(s);
}

static void on_report(spi_t *s, const uint8_t *b, size_t n) {
    pthread_mutex_lock(&s->mu);
    const uint8_t *inner = NULL;
    bool txs_shape = n == HP_RF_REMOTE_FRAME_LEN && b[0] == REPORT_REMOTE_DATA && b[2] == TX_STATUS_ID;
    if (txs_shape && (s->role == HP_RF_SPI_ROLE_REMOTE || b[1] == s->slot)) {
        if (s->txs_n == TXSQ) { s->txs_head = (s->txs_head + 1) % TXSQ; s->txs_n--; }
        uint8_t *e = s->txs[(s->txs_head + s->txs_n++) % TXSQ];
        e[0] = s->role == HP_RF_SPI_ROLE_REMOTE ? (uint8_t)(b[1] & 0x0F) : b[1];
        e[1] = b[3];
        push_event(s);
        pthread_mutex_unlock(&s->mu);
        return;
    }
    if (s->role == HP_RF_SPI_ROLE_HUB) {
        if (n == HP_RF_HUB_FRAME_LEN && b[0] == REPORT_HUB_DATA) inner = b + 2;
    } else if (n == HP_RF_REMOTE_FRAME_LEN && b[0] == REPORT_REMOTE_DATA && b[2] == 0x12) {
        inner = b + 4;
    }
    if (inner) {
        s->attached = true;
        if (s->qn == RXQ) { /* reader outran the hub loop: drop the oldest, keep the freshest */
            s->qhead = (s->qhead + 1) % RXQ;
            s->qn--;
        }
        memcpy(s->q[(s->qhead + s->qn++) % RXQ], inner, HP_RF_INNER_LEN);
        push_event(s);
    } else if ((b[0] == 0x10 || b[0] == 0x11) && n >= 5 && b[1] == s->slot && b[2] == 0x41 && b[3] == 0x04) {
        int up = !(b[4] & LINK_LOST);
        if (s->link_logged != up) {
            s->link_logged = up;
            if (up && n >= 7) slogf(s, "rf_spi: link up (slot %u, status 0x%02x, wpid %02x%02x)", (unsigned)b[1], (unsigned)b[4], (unsigned)b[6], (unsigned)b[5]);
            else if (up) slogf(s, "rf_spi: link up (slot %u, status 0x%02x)", (unsigned)b[1], (unsigned)b[4]);
            else slogf(s, "rf_spi: link lost (status 0x%02x)", (unsigned)b[4]);
        }
        if (b[4] & LINK_LOST) {
            if (s->attached) {
                s->attached = false;
                s->lost_pending = true;
                push_event(s);
            }
        } else if (!s->attached) {
            s->attached = true;
            push_event(s);
        }
    } else {
        logf_limited(s, (b[0] == 0x10 || b[0] == 0x11) ? "control report dropped" : "unexpected report dropped", b, n);
    }
    pthread_mutex_unlock(&s->mu);
}

static void *reader_main(void *arg) {
    spi_t *s = arg;
    uint8_t buf[READ_BUF];
    while (!s->stop) {
        ssize_t n = s->ops.read(s->ops.ctx, s->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno != EINTR && !s->stop) { /* a dead device must not spin */
                struct timespec ts = {0, 20 * 1000000L};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        if (n == 0) continue;
        dbg_frame(s, "RX", (long)n, buf, n > RX_MAX ? RX_MAX : (size_t)n);
        if (n > RX_MAX) { /* the driver's entries are 64 bytes: anything else is not a frame */
            logf_limited(s, "oversize read dropped", buf, RX_MAX);
            continue;
        }
        on_report(s, buf, (size_t)n);
    }
    pthread_mutex_lock(&s->mu);
    s->reader_done = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

/* ---- transport ---- */

static int spi_claim(void *self) {
    spi_t *s = self;
    pthread_mutex_lock(&s->mu);
    int rc = s->claimed ? -HP_E_BUSY : 0;
    s->claimed = true;
    pthread_mutex_unlock(&s->mu);
    return rc;
}

static int spi_send(void *self, const uint8_t *frame, size_t len) {
    spi_t *s = self;
    if (!s->claimed) return -HP_E_INVAL;
    if (!frame || !len) return -HP_E_INVAL;
    if (len > HP_RF_INNER_LEN) return -HP_E_TOOBIG;
    hp_rf_inner_t in;
    memset(in.b, 0, sizeof in.b); /* a short block is zero padded */
    memcpy(in.b, frame, len);
    hp_rf_frame_t w;
    if (s->role == HP_RF_SPI_ROLE_REMOTE) hp_rf_wrap_remote(s->slot, s->powermode, &in, &w);
    else hp_rf_wrap_hub(s->slot, &in, &w);
    return spi_write(s, w.data, w.len);
}

static void drain_wake(spi_t *s) {
    uint8_t junk[16];
    while (read(s->wake[0], junk, sizeof junk) > 0) {}
}

static int spi_recv(void *self, uint8_t *frame, size_t cap, int timeout_ms) {
    spi_t *s = self;
    if (!s->claimed) return -HP_E_INVAL;
    struct timespec until = {0, 0};
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += timeout_ms / 1000;
        until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    int rc = 0;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        if (s->qn) {
            if (cap < HP_RF_INNER_LEN) {
                rc = -HP_E_TOOBIG; /* dropped: an unreturnable frame must not wedge the queue */
            } else {
                memcpy(frame, s->q[s->qhead], HP_RF_INNER_LEN);
                rc = HP_RF_INNER_LEN;
            }
            s->qhead = (s->qhead + 1) % RXQ;
            s->qn--;
            break;
        }
        if (s->lost_pending) {
            s->lost_pending = false;
            rc = -HP_E_NOENT;
            break;
        }
        drain_wake(s);
        if (timeout_ms == 0) break;
        if (timeout_ms < 0) pthread_cond_wait(&s->cv, &s->mu);
        else if (pthread_cond_timedwait(&s->cv, &s->mu, &until) != 0) break;
    }
    pthread_mutex_unlock(&s->mu);
    return rc;
}

static int spi_tx_status(void *self, uint8_t *slot, uint8_t *status) {
    spi_t *s = self;
    int got = 0;
    pthread_mutex_lock(&s->mu);
    if (s->txs_n) {
        *slot = s->txs[s->txs_head][0];
        *status = s->txs[s->txs_head][1];
        s->txs_head = (s->txs_head + 1) % TXSQ;
        s->txs_n--;
        got = 1;
    }
    pthread_mutex_unlock(&s->mu);
    return got;
}

static bool spi_attached(void *self) {
    spi_t *s = self;
    pthread_mutex_lock(&s->mu);
    bool a = s->attached;
    pthread_mutex_unlock(&s->mu);
    return a;
}

/* ---- lifecycle ---- */

int hp_rf_spi_open(const hp_rf_spi_cfg_t *cfg, hp_rf_transport **out) {
    if (!out) return -HP_E_INVAL;
    spi_t *s = calloc(1, sizeof *s);
    if (!s) return -HP_E_IO;
    s->ops = (cfg && cfg->ops) ? *cfg->ops : hp_rf_spi_real_ops();
    s->role = cfg ? cfg->role : HP_RF_SPI_ROLE_HUB;
    if (s->role != HP_RF_SPI_ROLE_HUB && s->role != HP_RF_SPI_ROLE_REMOTE) {
        free(s);
        return -HP_E_INVAL;
    }
    s->powermode = cfg ? (uint8_t)(cfg->powermode & 0xF0) : 0; /* upper nibble only */
    s->slot = (cfg && cfg->slot) ? cfg->slot : s->role == HP_RF_SPI_ROLE_REMOTE ? HP_RF_SPI_REMOTE_SLOT : HP_RF_SPI_SLOT;
    s->fd = -1;
    s->link_logged = -1;
    if (cfg) {
        s->debug = cfg->debug;
        s->log = cfg->log;
        s->log_ctx = cfg->log_ctx;
    }
    s->t0 = s->ops.now_ms ? s->ops.now_ms(s->ops.ctx) : 0;
    s->wake[0] = s->wake[1] = -1;
    const char *dev = (cfg && cfg->dev_path) ? cfg->dev_path : HP_RF_SPI_DEV;

    /* Fail closed: alive or indeterminate (or no way to ask) both refuse. */
    int hal = s->ops.hal_alive ? s->ops.hal_alive(s->ops.ctx) : -1;
    if (hal != 0) {
        fprintf(stderr, "rf_spi: refusing to open %s: stock hal %s; run `tools/deploy_hub.py quiesce` first\n", dev,
                hal == 1 ? "is running" : "could not be ruled out (cannot read /proc)");
        free(s);
        return -HP_E_HALBUSY;
    }
    errno = 0;
    s->fd = s->ops.open(s->ops.ctx, dev, O_RDWR);
    if (s->fd < 0) {
        int rc = (errno == EBUSY || errno == EACCES) ? -HP_E_HALBUSY : errno == ENOENT ? -HP_E_NOENT : -HP_E_IO;
        free(s);
        return rc;
    }
    if (pipe(s->wake) < 0) goto fail_fd;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(s->wake[i], F_GETFL);
        fcntl(s->wake[i], F_SETFL, fl | O_NONBLOCK);
        fcntl(s->wake[i], F_SETFD, FD_CLOEXEC);
    }
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_mutex_init(&s->tx_mu, NULL);
    install_usr1();
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    int pr = pthread_create(&s->reader, &at, reader_main, s);
    pthread_attr_destroy(&at);
    if (pr != 0) goto fail_pipe;
    s->t = (hp_rf_transport){s, spi_send, spi_recv, spi_claim, spi_attached, spi_tx_status};
    if (spi_write(s, CMD_INIT, sizeof CMD_INIT) < 0)
        fprintf(stderr, "rf_spi: init command failed (the chip's ready line may be stuck); continuing\n");
    *out = &s->t;
    return 0;
fail_pipe:
    close(s->wake[0]);
    close(s->wake[1]);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->tx_mu);
fail_fd:
    (void)s->ops.close(s->ops.ctx, s->fd);
    free(s);
    return -HP_E_IO;
}

void hp_rf_spi_destroy(hp_rf_transport *t) {
    if (!t || t->recv != spi_recv) return;
    spi_t *s = t->self;
    s->stop = 1;
    /* The signal can land before the reader enters read() and be lost, so repeat until it exits. */
    bool done = false;
    for (int i = 0; i < 150 && !done; i++) {
        s->ops.interrupt(s->ops.ctx, s->reader);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 20 * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&s->mu);
        while (!s->reader_done && pthread_cond_timedwait(&s->cv, &s->mu, &until) == 0) {}
        done = s->reader_done;
        pthread_mutex_unlock(&s->mu);
    }
    if (!done) { /* a reader that ignores signals: leak rather than free memory it still uses */
        fprintf(stderr, "rf_spi: reader did not stop; leaking the transport\n");
        pthread_detach(s->reader);
        return;
    }
    pthread_join(s->reader, NULL);
    (void)s->ops.close(s->ops.ctx, s->fd);
    close(s->wake[0]);
    close(s->wake[1]);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->tx_mu);
    free(s);
}

int hp_rf_spi_wake_fd(const hp_rf_transport *t) {
    if (!t || t->recv != spi_recv) return -1;
    return ((const spi_t *)t->self)->wake[0];
}

int hp_rf_spi_set_pairing(hp_rf_transport *t, unsigned seconds) {
    if (!t || t->recv != spi_recv || ((spi_t *)t->self)->role != HP_RF_SPI_ROLE_HUB || seconds < 1 || seconds > 255)
        return -HP_E_INVAL; /* host-chip operation */
    const uint8_t cmd[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x00, (uint8_t)seconds};
    return spi_write(t->self, cmd, sizeof cmd);
}

int hp_rf_spi_unpair(hp_rf_transport *t, unsigned slot) {
    if (!t || t->recv != spi_recv || ((spi_t *)t->self)->role != HP_RF_SPI_ROLE_HUB || slot < 1 || slot > 15)
        return -HP_E_INVAL; /* host-chip operation */
    const uint8_t cmd[7] = {0x10, 0xFF, 0x80, 0xB2, 0x03, (uint8_t)slot, 0x00};
    return spi_write(t->self, cmd, sizeof cmd);
}

int hp_rf_spi_write_for_test(hp_rf_transport *t, const uint8_t *buf, size_t n) {
    if (!t || t->recv != spi_recv || !buf) return -HP_E_INVAL;
    return spi_write(t->self, buf, n);
}
