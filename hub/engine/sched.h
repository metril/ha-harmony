#ifndef HP_ENGINE_SCHED_H
#define HP_ENGINE_SCHED_H

/* Small fixed-capacity timer queue driven by an injectable monotonic clock
 * (milliseconds, uint32, wraparound-safe). Nothing here sleeps or blocks: the
 * owner polls with hp_sched_next_timeout_ms() and calls hp_sched_run().
 * Callbacks may schedule or cancel timers (including their own). Timers due at
 * the same time run in scheduling order. A timer scheduled by a callback with
 * delay 0 runs on the next hp_sched_run call, never in the same one. */

#include <stdint.h>

#define HP_SCHED_CAP 16

typedef uint32_t hp_sched_handle; /* 0 = invalid */
typedef void (*hp_sched_cb)(void *arg, uint32_t now_ms);
typedef uint32_t (*hp_clock_fn)(void *ctx);

typedef struct {
    hp_clock_fn clock;
    void *clock_ctx;
    uint32_t seq; /* next ordering stamp */
    uint32_t gen; /* handle generation */
    struct {
        int used;
        uint32_t due, seq, handle;
        hp_sched_cb cb;
        void *arg;
    } slot[HP_SCHED_CAP];
} hp_sched_t;

void hp_sched_init(hp_sched_t *, hp_clock_fn clock, void *clock_ctx);
uint32_t hp_sched_now(const hp_sched_t *);
/* Runs cb(arg, now) at/after now + delay_ms. Returns a handle, or 0 when full. */
hp_sched_handle hp_sched_after(hp_sched_t *, uint32_t delay_ms, hp_sched_cb cb, void *arg);
/* 1 if a pending timer was cancelled, 0 for a stale/unknown/zero handle. */
int hp_sched_cancel(hp_sched_t *, hp_sched_handle);
/* Milliseconds until the earliest timer (0 if overdue), -1 when none pending. */
int hp_sched_next_timeout_ms(const hp_sched_t *);
/* Runs every timer due at `now`. Returns how many ran. */
int hp_sched_run(hp_sched_t *, uint32_t now_ms);
int hp_sched_pending(const hp_sched_t *);

#endif
