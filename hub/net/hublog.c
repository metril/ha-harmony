#define _GNU_SOURCE
#include "hub/net/hublog.h"
#include "hub/net/crash.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hub/hp_errors.h"

static hp_hublog_t *g_cur;

static uint32_t mono_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000)) : 0;
}

uint32_t hp_mono_ms(void) { return mono_ms(); }

static bool stall_log(const char *what, int32_t late) {
    if (late <= (int32_t)HP_STALL_MS) return false; /* signed: an early wake or a clock step logs nothing */
    hp_log("hub: loop stall %u ms in %s\n", (unsigned)late, what);
    return true;
}

bool hp_stall_check(const char *what, uint32_t t0, uint32_t now) { return stall_log(what, (int32_t)(now - t0)); }

bool hp_stall_poll(uint32_t t0, uint32_t t1, int wait_ms, uint32_t cb_ms) {
    if (wait_ms < 0) wait_ms = 0; /* slept until woken: any wake is on time, but the callbacks still ran */
    /* Overrun past the requested wait and the callback time; negative when poll woke early. */
    return stall_log("poll wake", (int32_t)(t1 - t0) - wait_ms - (int32_t)cb_ms);
}

bool hp_stall_end(const char *what, uint32_t t0) { return hp_stall_check(what, t0, mono_ms()); }

int hp_hublog_init(hp_hublog_t *l, hp_log_emit_fn emit, void *ctx) {
    memset(l, 0, sizeof *l);
    l->wake[0] = l->wake[1] = -1;
    l->out_fd = STDERR_FILENO;
    l->now_ms = mono_ms;
    if (pthread_mutex_init(&l->mu, NULL) != 0 || pipe2(l->wake, O_NONBLOCK | O_CLOEXEC) < 0) return -HP_E_INVAL;
    l->emit = emit;
    l->ctx = ctx;
    l->tokens_m = l->wtokens_m = HP_LOG_BURST * 1000u;
    l->last_ms = l->now_ms();
    return 0;
}

void hp_hublog_install(hp_hublog_t *l) { g_cur = l; }
int hp_hublog_wake_fd(const hp_hublog_t *l) { return l->wake[0]; }

const char *hp_hublog_level(const char *m) {
    static const char *const w[] = {"cannot", "failed", "error", "giving up", "dropping", "refus", "unusable", "stall", "dropped", NULL};
    for (int i = 0; w[i]; i++)
        if (strstr(m, w[i])) return "warning";
    return "info";
}

/* "[HH:MM:SS.mmm +<uptime ms>ms] " (UTC wall clock, then the monotonic ms the stall lines and `ts` use). Returns its length. */
static size_t stamp_prefix(char *b, size_t cap, uint32_t ts) {
    struct timespec t;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0 || !gmtime_r(&t.tv_sec, &tm)) return 0;
    int k = snprintf(b, cap, "[%02d:%02d:%02d.%03d +%ums] ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(t.tv_nsec / 1000000),
                     (unsigned)ts);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

static bool is_warn(const char *s, size_t n) {
    char t[HP_LOG_LINE];
    if (n >= sizeof t) n = sizeof t - 1;
    memcpy(t, s, n);
    t[n] = 0;
    return hp_hublog_level(t)[0] == 'w';
}

static void ring_remove(hp_hublog_t *l, unsigned i) { /* logical index; mu held */
    for (; i + 1 < l->count; i++) l->ring[(l->head + i) % HP_LOG_RING] = l->ring[(l->head + i + 1) % HP_LOG_RING];
    l->count--;
}

static void push(hp_hublog_t *l, const char *s, size_t n, uint32_t ts) { /* mu held */
    char pre[40];
    size_t pn = l->stamp ? stamp_prefix(pre, sizeof pre, ts) : 0;
    if (pn + n >= HP_LOG_LINE) n = HP_LOG_LINE - 1 - pn;
    bool warn = is_warn(s, n);
    if (l->count == HP_LOG_RING) { /* overflow: info lines go before warnings */
        unsigned i = 0;
        while (i < l->count && l->ring[(l->head + i) % HP_LOG_RING].level[0] == 'w') i++;
        if (i < l->count) {
            ring_remove(l, i);
        } else if (!warn) { /* ring is all warnings: the incoming info line is the one to lose */
            l->dropped++;
            return;
        } else {
            l->head = (l->head + 1) % HP_LOG_RING;
            l->count--;
        }
        l->dropped++;
    }
    hp_logline_t *e = &l->ring[(l->head + l->count++) % HP_LOG_RING];
    memcpy(e->msg, pre, pn);
    memcpy(e->msg + pn, s, n);
    e->msg[pn + n] = 0;
    snprintf(e->level, sizeof e->level, "%s", hp_hublog_level(e->msg));
    e->ts = ts;
    e->dropped = 0;
}

/* One finished line into the ring. Never blocks: a busy lock drops (and counts) the line. */
static void ship(hp_hublog_t *l, const char *s, size_t n) {
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    if (!l || !n) return;
    if (pthread_mutex_trylock(&l->mu) != 0) {
        atomic_fetch_add(&l->lost, 1);
        return;
    }
    uint32_t now = l->now_ms();
    uint64_t add = (uint64_t)(now - l->last_ms) * HP_LOG_RATE; /* milli-tokens: rate/s * ms */
    l->last_ms = now;
    bool warn = is_warn(s, n); /* warnings draw from their own bucket: an info flood cannot starve them */
    uint32_t *tok = warn ? &l->wtokens_m : &l->tokens_m;
    uint32_t *sup = warn ? &l->wsuppressed : &l->suppressed;
    uint64_t t = *tok + add;
    uint64_t o = (warn ? l->tokens_m : l->wtokens_m) + add; /* the other bucket refills too */
    if (t > HP_LOG_BURST * 1000u) t = HP_LOG_BURST * 1000u;
    if (o > HP_LOG_BURST * 1000u) o = HP_LOG_BURST * 1000u;
    if (warn) l->tokens_m = (uint32_t)o;
    else l->wtokens_m = (uint32_t)o;
    bool kick = false;
    if (t < 1000) {
        (*sup)++;
    } else {
        t -= 1000;
        if (*sup) {
            char sum[64];
            int sn = snprintf(sum, sizeof sum, "hub: %u %slog lines suppressed", (unsigned)*sup, warn ? "warning " : "");
            push(l, sum, (size_t)sn, now);
            *sup = 0;
        }
        push(l, s, n, now);
        kick = !l->woken;
        if (kick) l->woken = true;
    }
    *tok = (uint32_t)t;
    pthread_mutex_unlock(&l->mu);
    if (kick) { char b = 1; (void)!write(l->wake[1], &b, 1); }
}

static void out_line(hp_hublog_t *l, const char *s, size_t n) { /* s ends in \n; one write() so lines never interleave */
    int fd = l ? l->out_fd : STDERR_FILENO;
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

void hp_log(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof b - 2) n = sizeof b - 2;
    if (!n || b[n - 1] != '\n') b[n++] = '\n';
    hp_crumb_raw(b, (size_t)n); /* the crash handler reports the last few lines */
    hp_hublog_t *l = g_cur;
    out_line(l, b, (size_t)n); /* first, synchronously: the file has it before anything else can go wrong */
    size_t st = 0;
    for (size_t i = 0; i < (size_t)n; i++)
        if (b[i] == '\n') {
            ship(l, b + st, i - st);
            st = i + 1;
        }
}

static void feed(hp_hublog_t *l, hp_linebuf_t *lb, const char *prefix, bool to_file, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            if (lb->n) {
                if (to_file) {
                    lb->part[lb->n] = '\n';
                    out_line(l, lb->part, lb->n + 1);
                    ship(l, lb->part, lb->n);
                } else {
                    char t[sizeof lb->part + 24];
                    int k = snprintf(t, sizeof t, "%s%.*s", prefix, (int)lb->n, lb->part);
                    ship(l, t, (size_t)k);
                }
            }
            lb->n = 0;
            lb->over = false;
        } else if (!lb->over) {
            if (lb->n < sizeof lb->part - 1) lb->part[lb->n++] = (unsigned char)c < 0x20 && c != '\t' ? ' ' : c;
            else lb->over = true;
        }
    }
}

void hp_hublog_feed(hp_hublog_t *l, hp_linebuf_t *lb, const char *buf, size_t n) { feed(l, lb, "", true, buf, n); }
void hp_hublog_feed_ship(hp_hublog_t *l, hp_linebuf_t *lb, const char *prefix, const char *buf, size_t n) {
    feed(l, lb, prefix, false, buf, n);
}
void hp_log_feed(hp_linebuf_t *lb, const char *buf, size_t n) { hp_hublog_feed(g_cur, lb, buf, n); }
void hp_log_feed_ship(hp_linebuf_t *lb, const char *prefix, const char *buf, size_t n) {
    hp_hublog_feed_ship(g_cur, lb, prefix, buf, n);
}

bool hp_hublog_readable(const hp_hublog_t *l) {
    struct pollfd p = {l->wake[0], POLLIN, 0};
    return l->wake[0] >= 0 && poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

void hp_hublog_service(hp_hublog_t *l) {
    char junk[16];
    pthread_mutex_lock(&l->mu);
    l->woken = false;
    pthread_mutex_unlock(&l->mu);
    while (read(l->wake[0], junk, sizeof junk) > 0) {}
    for (;;) {
        hp_logline_t e;
        pthread_mutex_lock(&l->mu);
        if (!l->count) { pthread_mutex_unlock(&l->mu); return; }
        e = l->ring[l->head];
        l->head = (l->head + 1) % HP_LOG_RING;
        l->count--;
        e.dropped = l->dropped + atomic_exchange(&l->lost, 0);
        l->dropped = 0;
        pthread_mutex_unlock(&l->mu);
        if (l->emit && l->emit(l->ctx, &e)) continue;
        pthread_mutex_lock(&l->mu); /* nobody to tell: put it back at the front (or count it lost if the ring refilled) */
        if (l->count < HP_LOG_RING) {
            l->head = (l->head + HP_LOG_RING - 1) % HP_LOG_RING;
            l->ring[l->head] = e;
            l->count++;
            l->dropped += e.dropped;
        } else {
            l->dropped += e.dropped + 1;
        }
        pthread_mutex_unlock(&l->mu);
        return;
    }
}
