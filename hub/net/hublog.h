#ifndef HP_HUBLOG_H
#define HP_HUBLOG_H

/* Ships the daemon's stderr lines to Home Assistant (ws `hub_log`, proto/schema.yaml). Nothing is stored on the hub:
 * a reader thread copies fd 2 straight through to the original stderr (the tmpfs log, so a freeze still leaves every
 * line there) and queues each line in a bounded in-memory ring; the poll loop wakes on wake_fd and hands the ring to
 * the emit callback, in order. While the callback reports no taker (no authenticated client) the lines stay in the
 * ring, oldest dropped first (counted, reported with the next line), and are sent after the next hello. No timers. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_LOG_RING 64
#define HP_LOG_LINE 192 /* bytes kept per line (longer lines are cut) */

typedef struct {
    char level[8]; /* "info" | "warning" */
    char msg[HP_LOG_LINE];
    uint32_t ts; /* hub uptime, ms */
    uint32_t dropped; /* lines lost to a full ring since the previous emitted one */
} hp_logline_t;

/* true = delivered to at least one client (the line is consumed); false = keep it queued. */
typedef bool (*hp_log_emit_fn)(void *ctx, const hp_logline_t *l);

typedef struct {
    pthread_mutex_t mu;
    hp_logline_t ring[HP_LOG_RING];
    unsigned head, count;
    uint32_t dropped;
    char part[HP_LOG_LINE]; /* the unfinished line */
    size_t plen;
    bool over; /* the unfinished line is already cut: skip to its newline */
    bool woken;
    int wake[2];
    hp_log_emit_fn emit;
    void *ctx;
    int orig_fd, pipe_r, pipe_w;
    pthread_t th;
    bool running, quit;
} hp_hublog_t;

int hp_hublog_init(hp_hublog_t *, hp_log_emit_fn, void *ctx); /* 0 or -HP_E_* */
/* Feeds raw bytes (any split); finished lines go to the ring and wake the poll loop. Thread-safe. */
void hp_hublog_write(hp_hublog_t *, const char *buf, size_t n);
/* Poll this fd for readability; then call hp_hublog_service. -1 before init. */
int hp_hublog_wake_fd(const hp_hublog_t *);
/* Event-loop thread: emits queued lines in order until the callback declines. Also call it after a client authenticates. */
void hp_hublog_service(hp_hublog_t *);
/* Level of a line: "warning" for failure wording, else "info". */
const char *hp_hublog_level(const char *msg);
/* Replaces fd 2 with a pipe drained by a reader thread that tees to the old stderr and calls hp_hublog_write. */
int hp_hublog_capture(hp_hublog_t *);
/* Restores fd 2 and stops the reader. */
void hp_hublog_release(hp_hublog_t *);

#endif
