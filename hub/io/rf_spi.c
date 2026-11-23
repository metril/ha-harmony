#define _POSIX_C_SOURCE 200809L
#include "hub/net/hublog.h"
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
#include "hub/rf/rf_payload.h"

#define RXQ 16        /* received inner blocks buffered for recv() */
#define READ_BUF 128  /* the driver ignores `count` and copies a 64-byte entry: never pass less */
#define RX_MAX 64
#define LOG_MAX 32    /* diagnostics per message kind; refilled on every link-up */
#define FLAP_BURST 4        /* link up/lost lines logged per flap episode */
#define FLAP_QUIET_MS 10000 /* a flap this long after the previous one starts a new episode */
#define READ_BACKOFF_MIN_MS 20
#define READ_BACKOFF_MAX_MS 2000
enum { LK_CTRL, LK_UNEXP, LK_OVERSIZE, LK_N };
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
    uint8_t slot;       /* peer slot; valid only while slot_known (guarded by mu) */
    bool slot_known;    /* configured, or learned from a link-up report */
    bool slot_fixed;    /* configured: never overridden by the radio */
    char slot_file[256]; /* persisted learned slot ("" = none) */
    uint8_t persisted;  /* what the file holds (0 = nothing) */
    hp_rf_spi_role_t role;
    uint8_t powermode;
    int fd;
    bool claimed;

    pthread_mutex_t mu; /* guards the receive state below */
    pthread_cond_t cv;
    uint8_t q[RXQ][HP_RF_INNER_LEN];
    uint32_t q_at[RXQ];   /* arrival time (reader thread) of each queued frame */
    bool q_lost[RXQ];     /* the entry is a link-lost marker, not a frame: loss is ordered with the frames */
    uint32_t last_rx_at;  /* arrival time of the frame recv() returned last */
    size_t qhead, qn;
    bool attached;
    int wake[2]; /* self-pipe: readable while recv() has something to return */
    unsigned logged[LK_N]; /* reader thread only */
    /* link-flap limiter (reader thread only) */
    uint64_t flap_last, flap_start;
    unsigned flap_n, flap_supp;
    bool reader_done;
    pthread_t reader;
    volatile sig_atomic_t stop;

    pthread_mutex_t tx_mu; /* serialises write() and its backoff state */
    uint64_t next_ok;
    unsigned delay_ms;
    uint32_t last_tx_at;  /* when the last successful write() returned */

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

/* Lines produced while `mu` is held are queued here and written after the unlock: logging and the slot file
 * (fopen/rename on flash) must not stall the main thread, which takes `mu` every loop iteration. */
static void emit_line(spi_t *s, const char *line);
#define DEFER_LINES 6
typedef struct {
    char line[DEFER_LINES][160];
    size_t n;
    bool persist;
    uint8_t pslot;
} defer_t;

static void dq(defer_t *d, const char *line) {
    if (d->n < DEFER_LINES) {
        snprintf(d->line[d->n], sizeof d->line[0], "%s", line);
        d->n++;
    }
}

static void logf_limited(spi_t *s, defer_t *d, int kind, const char *what, const uint8_t *b, size_t n) {
    if (s->logged[kind] >= LOG_MAX) return;
    s->logged[kind]++;
    char line[96];
    int k = snprintf(line, sizeof line, "rf_spi: %s (%zu bytes):", what, n);
    for (size_t i = 0; i < n && i < 8 && k > 0 && (size_t)k + 4 < sizeof line; i++) k += snprintf(line + k, sizeof line - (size_t)k, " %02x", b[i]);
    if (d) dq(d, line);
    else emit_line(s, line);
}

/* Diagnostics sink (default stderr). */
static void emit_line(spi_t *s, const char *line) {
    if (s->log) s->log(s->log_ctx, line);
    else hp_log("%s\n", line);
}

static void slogf(spi_t *s, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit_line(s, line);
}

/* A link up/lost line, rate-limited: the first FLAP_BURST of an episode go out; the rest are counted, and the
 * next flap after FLAP_QUIET_MS of quiet reports "N flaps in last Xs" first. Event-driven: no timer. */
static void flap_logf(spi_t *s, defer_t *d, const char *fmt, ...) {
    uint64_t now = s->ops.now_ms(s->ops.ctx);
    if (s->flap_n && now - s->flap_last >= FLAP_QUIET_MS) {
        if (s->flap_supp) {
            char sum[96];
            snprintf(sum, sizeof sum, "rf_spi: %u more link flaps suppressed in last %llus", s->flap_supp,
                     (unsigned long long)((s->flap_last - s->flap_start) / 1000u + 1u));
            dq(d, sum);
        }
        s->flap_n = s->flap_supp = 0;
    }
    if (!s->flap_n) s->flap_start = now;
    s->flap_last = now;
    if (s->flap_n >= FLAP_BURST) {
        s->flap_supp++;
        return;
    }
    s->flap_n++;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dq(d, line);
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
static int spi_write_ex(spi_t *s, const uint8_t *buf, size_t n, bool track) {
    if (s->role == HP_RF_SPI_ROLE_REMOTE && n > REMOTE_WRITE_CAP) return -HP_E_TOOBIG; /* never reaches the syscall */
    pthread_mutex_lock(&s->tx_mu);
    uint64_t now = s->ops.now_ms(s->ops.ctx);
    int rc = 0;
    if (s->delay_ms && now < s->next_ok) {
        rc = -HP_E_BUSY;
    } else {
        ssize_t w;
        uint32_t t0 = hp_mono_ms();
        do w = s->ops.write(s->ops.ctx, s->fd, buf, n);
        while (w < 0 && errno == EINTR);
        hp_stall_end("rf_spi write", t0); /* a blocked SPI write() stalls the event loop that called it */
        dbg_frame(s, "TX", (long)w, buf, n);
        if (w < 0 && !track) {
            rc = -HP_E_IO; /* an untracked write (sync pulse) must not put the next real send into backoff */
        } else if (w < 0) {
            unsigned d = s->delay_ms ? s->delay_ms * 2 : HP_RF_SPI_BACKOFF_MIN_MS;
            s->delay_ms = d > HP_RF_SPI_BACKOFF_MAX_MS ? HP_RF_SPI_BACKOFF_MAX_MS : d;
            s->next_ok = now + s->delay_ms;
            rc = -HP_E_IO;
        } else {
            s->delay_ms = 0;
            s->last_tx_at = (uint32_t)s->ops.now_ms(s->ops.ctx);
        }
    }
    pthread_mutex_unlock(&s->tx_mu);
    return rc;
}

static int spi_write(spi_t *s, const uint8_t *buf, size_t n) { return spi_write_ex(s, buf, n, true); }

/* ---- reader ---- */

static void push_event(spi_t *s) {
    pthread_cond_broadcast(&s->cv);
    wake(s);
}

/* Appends an entry (frame or lost marker). When full the oldest evictable entry goes (the freshest data is kept):
 * a lost marker and a key RELEASE frame are evicted only when nothing else is queued (a lost release leaves a key
 * held forever). Caller holds mu. */
static bool q_is_release(const spi_t *s, size_t i) {
    size_t a = (s->qhead + i) % RXQ;
    if (s->q_lost[a]) return false;
    const uint8_t *b = s->q[a]; /* first (only) block of a key_event object: kind|first|seq, type, idx 0, len, body */
    if ((b[0] & 0xE0) != HP_RF_KIND_OBJECT || !(b[0] & HP_RF_FLAG_FIRST) || b[1] != HP_MSG_KEY_EVENT || b[3] > HP_RF_CHUNK - 1)
        return false;
    hp_rf_val_t v[3];
    return hp_rf_unpack(HP_MSG_KEY_EVENT, b + 4, b[3], v, 3) == 0 && v[2].u == HP_KEY_ACTION_RELEASE;
}

static size_t q_push(spi_t *s, bool lost) {
    if (s->qn == RXQ) {
        size_t drop = 0; /* offset from the head */
        while (drop < s->qn && (s->q_lost[(s->qhead + drop) % RXQ] || q_is_release(s, drop))) drop++;
        if (drop == s->qn) { /* only markers and releases queued: drop the oldest frame, markers last */
            drop = 0;
            while (drop < s->qn && s->q_lost[(s->qhead + drop) % RXQ]) drop++;
            if (drop == s->qn) drop = 0;
        }
        for (size_t i = drop; i + 1 < s->qn; i++) { /* close the gap, keeping the order */
            size_t a = (s->qhead + i) % RXQ, b = (s->qhead + i + 1) % RXQ;
            memcpy(s->q[a], s->q[b], HP_RF_INNER_LEN);
            s->q_at[a] = s->q_at[b];
            s->q_lost[a] = s->q_lost[b];
        }
        s->qn--;
    }
    size_t qi = (s->qhead + s->qn++) % RXQ;
    s->q_lost[qi] = lost;
    return qi;
}

/* Persist the learned slot (tmp + rename; best effort): a restart on an already-up link gets no link-up
 * report, so the last known slot is the provisional start value. Reader thread, `mu` NOT held (file I/O). */
static void persist_slot(spi_t *s, uint8_t slot) {
    if (!s->slot_file[0] || s->persisted == slot) return;
    char tmp[sizeof s->slot_file + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->slot_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    int ok = fprintf(f, "%u\n", (unsigned)slot) > 0;
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, s->slot_file) == 0) s->persisted = slot;
    else unlink(tmp);
}

static void load_slot(spi_t *s, const char *path) {
    if (!path || !*path || strlen(path) >= sizeof s->slot_file) return;
    snprintf(s->slot_file, sizeof s->slot_file, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned v = 0;
    char c = 0;
    int n = fscanf(f, "%u%c", &v, &c);
    fclose(f);
    if (n >= 1 && v >= 1 && v <= 15 && (n == 1 || c == '\n')) {
        s->slot = (uint8_t)v;
        s->slot_known = true; /* provisional: a report or frame on another slot corrects it while detached */
        s->persisted = (uint8_t)v;
    }
}

/* The slot comes from the radio unless configured: a link-up report (`10 NN 41 04 <status> <wpid>`, loss
 * bit clear) or the first data frame (hub `0x12 NN ...`, remote `0x20 <pipe> 0x12 ...`, slot = pipe & 0x0F),
 * because a restart on an already-up link gets no link-up report. A slot is only taken while detached, so a
 * second remote cannot hijack a live link, and a re-link on another slot is picked up. The wpid is not
 * checked: the hub pairs one remote. Caller holds mu; output goes to `d`. */
static void learn_slot(spi_t *s, defer_t *d, unsigned slot, const char *from) {
    if (s->slot_fixed || slot < 1 || slot > 15) return;
    if (s->slot_known && (s->slot == slot || s->attached)) return;
    s->slot = (uint8_t)slot;
    s->slot_known = true;
    s->txs_n = 0; /* statuses of the old slot */
    char line[96];
    snprintf(line, sizeof line, "rf_spi: slot %u learned from %s", slot, from);
    dq(d, line);
    d->persist = true;
    d->pslot = (uint8_t)slot;
}

/* After the unlock: the queued lines, then the slot file. */
static void flush_defer(spi_t *s, defer_t *d) {
    for (size_t i = 0; i < d->n; i++) {
        emit_line(s, d->line[i]);
    }
    if (d->persist) persist_slot(s, d->pslot);
}

static void on_report(spi_t *s, const uint8_t *b, size_t n) {
    defer_t d;
    d.n = 0;
    d.persist = false;
    pthread_mutex_lock(&s->mu);
    const uint8_t *inner = NULL;
    bool txs_shape = n == HP_RF_REMOTE_FRAME_LEN && b[0] == REPORT_REMOTE_DATA && b[2] == TX_STATUS_ID;
    if (txs_shape && !s->slot_known) learn_slot(s, &d, b[1] & 0x0F, "TX status");
    if (txs_shape && (s->role == HP_RF_SPI_ROLE_REMOTE || (s->slot_known && b[1] == s->slot))) {
        if (s->txs_n == TXSQ) { s->txs_head = (s->txs_head + 1) % TXSQ; s->txs_n--; }
        uint8_t *e = s->txs[(s->txs_head + s->txs_n++) % TXSQ];
        e[0] = s->role == HP_RF_SPI_ROLE_REMOTE ? (uint8_t)(b[1] & 0x0F) : b[1];
        e[1] = b[3];
        push_event(s);
        pthread_mutex_unlock(&s->mu);
        flush_defer(s, &d);
        return;
    }
    if (s->role == HP_RF_SPI_ROLE_HUB) {
        /* only the configured peer's data: another slot's uplink must not look like our remote's
         * (same filter as the TX status above) */
        if (n == HP_RF_HUB_FRAME_LEN && b[0] == REPORT_HUB_DATA) {
            learn_slot(s, &d, b[1], "data frame");
            if (s->slot_known && b[1] == s->slot) inner = b + 2;
            else if (!s->slot_known && s->debug) dq(&d, "rf_spi: data dropped, slot not learned yet");
        }
    } else if (n == HP_RF_REMOTE_FRAME_LEN && b[0] == REPORT_REMOTE_DATA && b[2] == 0x12) {
        learn_slot(s, &d, b[1] & 0x0F, "data frame");
        if (s->slot_known) inner = b + 4;
        else if (s->debug) dq(&d, "rf_spi: data dropped, slot not learned yet");
    }
    if (n >= 5 && (b[0] == 0x10 || b[0] == 0x11) && b[2] == 0x41 && b[3] == 0x04 && !(b[4] & LINK_LOST))
        learn_slot(s, &d, b[1], "link report");
    if (inner) {
        if (!s->attached && s->link_logged == 0 && s->slot_known) { /* a data frame is the first sign the link is back */
            s->link_logged = 1;
            memset(s->logged, 0, sizeof s->logged); /* a new link: diagnostics get a fresh budget */
            flap_logf(s, &d, "rf_spi: link up (slot %u, data frame)", (unsigned)s->slot);
        }
        s->attached = true;
        size_t qi = q_push(s, false); /* a full queue (reader outran the hub loop) drops the oldest frame */
        memcpy(s->q[qi], inner, HP_RF_INNER_LEN);
        s->q_at[qi] = (uint32_t)s->ops.now_ms(s->ops.ctx);
        push_event(s);
    } else if ((b[0] == 0x10 || b[0] == 0x11) && n >= 5 && s->slot_known && b[1] == s->slot && b[2] == 0x41 && b[3] == 0x04) {
        int up = !(b[4] & LINK_LOST);
        if (s->link_logged != up) {
            s->link_logged = up;
            if (up) memset(s->logged, 0, sizeof s->logged);
            if (up && n >= 7) flap_logf(s, &d, "rf_spi: link up (slot %u, status 0x%02x, wpid %02x%02x)", (unsigned)b[1], (unsigned)b[4], (unsigned)b[6], (unsigned)b[5]);
            else if (up) flap_logf(s, &d, "rf_spi: link up (slot %u, status 0x%02x)", (unsigned)b[1], (unsigned)b[4]);
            else flap_logf(s, &d, "rf_spi: link lost (status 0x%02x)", (unsigned)b[4]);
        }
        if (b[4] & LINK_LOST) {
            if (s->attached) {
                s->attached = false;
                q_push(s, true); /* in sequence with the frames: those before it belong to the old link */
                push_event(s);
            }
        } else if (!s->attached) {
            s->attached = true;
            push_event(s);
        }
    } else {
        bool ctl = b[0] == 0x10 || b[0] == 0x11;
        logf_limited(s, &d, ctl ? LK_CTRL : LK_UNEXP, ctl ? "control report dropped" : "unexpected report dropped", b, n);
    }
    pthread_mutex_unlock(&s->mu);
    flush_defer(s, &d);
}

static void *reader_main(void *arg) {
    spi_t *s = arg;
    uint8_t buf[READ_BUF];
    unsigned backoff = 0; /* ms to wait after a read error; doubles to READ_BACKOFF_MAX_MS */
    while (!s->stop) {
        ssize_t n = s->ops.read(s->ops.ctx, s->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno != EINTR && !s->stop) { /* a dead device must not spin */
                if (!backoff) {
                    slogf(s, "rf_spi: read failed (errno %d); retrying with backoff", errno);
                    backoff = READ_BACKOFF_MIN_MS;
                } else if (backoff < READ_BACKOFF_MAX_MS) {
                    backoff = backoff * 2 > READ_BACKOFF_MAX_MS ? READ_BACKOFF_MAX_MS : backoff * 2;
                }
                struct timespec ts = {backoff / 1000, (long)(backoff % 1000) * 1000000L};
                nanosleep(&ts, NULL); /* a SIGUSR1 from destroy cuts it short */
            }
            continue;
        }
        if (backoff) {
            backoff = 0;
            slogf(s, "rf_spi: read recovered");
        }
        if (n == 0) continue;
        dbg_frame(s, "RX", (long)n, buf, n > RX_MAX ? RX_MAX : (size_t)n);
        if (n > RX_MAX) { /* the driver's entries are 64 bytes: anything else is not a frame */
            logf_limited(s, NULL, LK_OVERSIZE, "oversize read dropped", buf, RX_MAX);
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
    pthread_mutex_lock(&s->mu);
    bool known = s->slot_known;
    uint8_t slot = s->slot;
    pthread_mutex_unlock(&s->mu);
    if (!known) { /* never guess a slot: nothing is written before the radio has reported one */
        if (s->debug) slogf(s, "rf_spi: send dropped, slot not learned yet");
        return -HP_E_NOENT;
    }
    hp_rf_inner_t in;
    memset(in.b, 0, sizeof in.b); /* a short block is zero padded */
    memcpy(in.b, frame, len);
    hp_rf_frame_t w;
    if (s->role == HP_RF_SPI_ROLE_REMOTE) hp_rf_wrap_remote(slot, s->powermode, &in, &w);
    else hp_rf_wrap_hub(slot, &in, &w);
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
            if (s->q_lost[s->qhead]) {
                rc = -HP_E_NOENT; /* the link went away here: frames queued after this are the next link's */
            } else if (cap < HP_RF_INNER_LEN) {
                rc = -HP_E_TOOBIG; /* dropped: an unreturnable frame must not wedge the queue */
            } else {
                memcpy(frame, s->q[s->qhead], HP_RF_INNER_LEN);
                rc = HP_RF_INNER_LEN;
            }
            if (!s->q_lost[s->qhead]) s->last_rx_at = s->q_at[s->qhead];
            s->qhead = (s->qhead + 1) % RXQ;
            s->qn--;
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

static int spi_frame_times(void *self, uint32_t *rx_at, uint32_t *tx_at) {
    spi_t *s = self;
    pthread_mutex_lock(&s->mu);
    *rx_at = s->last_rx_at;
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_lock(&s->tx_mu);
    *tx_at = s->last_tx_at;
    pthread_mutex_unlock(&s->tx_mu);
    return 1;
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
    s->slot = (cfg && cfg->slot) ? cfg->slot : 0; /* 0 = learn it from the link-up report */
    s->slot_known = s->slot_fixed = s->slot != 0;
    if (!s->slot_fixed && cfg) load_slot(s, cfg->slot_file);
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
        hp_log("rf_spi: refusing to open %s: stock hal %s; run `tools/deploy_hub.py quiesce` first\n", dev,
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
    s->t = (hp_rf_transport){s, spi_send, spi_recv, spi_claim, spi_attached, spi_tx_status, spi_frame_times};
    if (spi_write(s, CMD_INIT, sizeof CMD_INIT) < 0)
        hp_log("rf_spi: init command failed (the chip's ready line may be stuck); continuing\n");
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
        hp_log("rf_spi: reader did not stop; leaking the transport\n");
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

int hp_rf_spi_sync(hp_rf_transport *t) {
    if (!t || t->recv != spi_recv || ((spi_t *)t->self)->role != HP_RF_SPI_ROLE_REMOTE) return -HP_E_INVAL;
    const uint8_t cmd[10] = {0x12, 0x01};
    return spi_write_ex(t->self, cmd, sizeof cmd, false); /* a failed pulse never blocks the key send behind it */
}

int hp_rf_spi_write_for_test(hp_rf_transport *t, const uint8_t *buf, size_t n) {
    if (!t || t->recv != spi_recv || !buf) return -HP_E_INVAL;
    return spi_write(t->self, buf, n);
}

int hp_rf_spi_slot(const hp_rf_transport *t) {
    if (!t || t->recv != spi_recv) return -1;
    spi_t *s = t->self;
    pthread_mutex_lock(&s->mu);
    int r = s->slot_known ? s->slot : -1;
    pthread_mutex_unlock(&s->mu);
    return r;
}
