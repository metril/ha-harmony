#ifndef HP_HUBLOG_H
#define HP_HUBLOG_H

/* Hub diagnostics: hp_log() replaces fprintf(stderr). Each line is
 *   1. write()n synchronously to the log fd (fd 2, the tmpfs file), exactly as before, so it survives a crash;
 *   2. pushed, without ever blocking, into a bounded in-memory ring (mutex try-lock: on contention or overflow the
 *      line is dropped from the ring and counted) for shipping to Home Assistant as `hub_log` (docs/proto/hub-log.md).
 * The poll loop wakes on wake_fd (event-driven, no timers) and hp_hublog_service hands the ring to the emit
 * callback in order. While the callback has no taker the lines stay queued (oldest dropped first, counted).
 * Shipping is rate limited by a token bucket (burst HP_LOG_BURST, HP_LOG_RATE lines/s); suppressed lines still reach
 * the file, and one "N log lines suppressed" line is shipped when the flow resumes. Nothing is persisted by the ring. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_LOG_RING 64
#define HP_LOG_LINE 192 /* bytes shipped per line (longer lines are cut; the file keeps them whole) */
#define HP_LOG_BURST 30 /* lines that may ship at once */
#define HP_LOG_RATE 10  /* lines per second afterwards */

typedef struct {
    char level[8]; /* "info" | "warning" */
    char msg[HP_LOG_LINE];
    uint32_t ts; /* hub uptime, ms */
    uint32_t dropped; /* lines lost (full ring, lock contention) since the previous emitted one */
} hp_logline_t;

/* true = delivered to at least one client (the line is consumed); false = keep it queued. */
typedef bool (*hp_log_emit_fn)(void *ctx, const hp_logline_t *l);

typedef struct {
    pthread_mutex_t mu;
    hp_logline_t ring[HP_LOG_RING];
    unsigned head, count;
    uint32_t dropped;
    _Atomic uint32_t lost; /* dropped on lock contention (counted without the lock) */
    uint32_t tokens_m, last_ms, suppressed; /* token bucket, milli-tokens */
    bool woken;
    int wake[2];
    int out_fd; /* where lines are written first (default 2) */
    uint32_t (*now_ms)(void); /* tests: the bucket's clock */
    hp_log_emit_fn emit;
    void *ctx;
} hp_hublog_t;

/* A partial line of a byte stream (the bt helper's stderr). */
typedef struct {
    char part[512];
    size_t n;
    bool over;
} hp_linebuf_t;

int hp_hublog_init(hp_hublog_t *, hp_log_emit_fn, void *ctx); /* 0 or -HP_E_* */
/* Makes hp_log ship through this instance (NULL = file only). */
void hp_hublog_install(hp_hublog_t *);
/* Poll this fd for readability; then call hp_hublog_service. -1 before init. */
int hp_hublog_wake_fd(const hp_hublog_t *);
/* Event-loop thread: emits queued lines in order until the callback declines. Also call it after a client authenticates. */
void hp_hublog_service(hp_hublog_t *);
/* Level of a line: "warning" for failure wording, else "info". */
const char *hp_hublog_level(const char *msg);

/* printf-style diagnostics line (newline added when missing). Never blocks on the ring; any thread. */
void hp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The same for bytes read from a child's stderr pipe: complete lines are written to the log fd and queued. */
void hp_hublog_feed(hp_hublog_t *, hp_linebuf_t *, const char *buf, size_t n);
/* Lines that already live in a file elsewhere (the boot hook's log): queued for shipping only, tagged with `prefix`. */
void hp_hublog_feed_ship(hp_hublog_t *, hp_linebuf_t *, const char *prefix, const char *buf, size_t n);
void hp_log_feed_ship(hp_linebuf_t *, const char *prefix, const char *buf, size_t n);
/* hp_hublog_feed on the installed instance. */
void hp_log_feed(hp_linebuf_t *, const char *buf, size_t n);

#endif
