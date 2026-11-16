#include "hub/engine/sched.h"

#include <string.h>

void hp_sched_init(hp_sched_t *s, hp_clock_fn clock, void *clock_ctx) {
    memset(s, 0, sizeof *s);
    s->clock = clock;
    s->clock_ctx = clock_ctx;
}

uint32_t hp_sched_now(const hp_sched_t *s) { return s->clock ? s->clock(s->clock_ctx) : 0; }

static int before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

hp_sched_handle hp_sched_after(hp_sched_t *s, uint32_t delay_ms, hp_sched_cb cb, void *arg) {
    if (!cb) return 0;
    for (int i = 0; i < HP_SCHED_CAP; i++) {
        if (s->slot[i].used) continue;
        if (++s->gen > 0xFFFFFF) s->gen = 1;
        s->slot[i].used = 1;
        s->slot[i].due = hp_sched_now(s) + delay_ms;
        s->slot[i].seq = s->seq++;
        s->slot[i].handle = (s->gen << 8) | (uint32_t)(i + 1);
        s->slot[i].cb = cb;
        s->slot[i].arg = arg;
        return s->slot[i].handle;
    }
    return 0;
}

int hp_sched_cancel(hp_sched_t *s, hp_sched_handle h) {
    unsigned i = h & 0xFF;
    if (!h || i == 0 || i > HP_SCHED_CAP) return 0;
    if (!s->slot[i - 1].used || s->slot[i - 1].handle != h) return 0;
    s->slot[i - 1].used = 0;
    return 1;
}

int hp_sched_pending(const hp_sched_t *s) {
    int n = 0;
    for (int i = 0; i < HP_SCHED_CAP; i++) n += s->slot[i].used;
    return n;
}

int hp_sched_next_timeout_ms(const hp_sched_t *s) {
    int found = 0;
    uint32_t best = 0;
    for (int i = 0; i < HP_SCHED_CAP; i++) {
        if (!s->slot[i].used) continue;
        if (!found || before(s->slot[i].due, best)) best = s->slot[i].due;
        found = 1;
    }
    if (!found) return -1;
    int32_t d = (int32_t)(best - hp_sched_now(s));
    if (d <= 0) return 0;
    return d > 0x7FFFFFF ? 0x7FFFFFF : (int)d;
}

int hp_sched_run(hp_sched_t *s, uint32_t now) {
    uint32_t limit = s->seq; /* timers scheduled from here on wait for the next run */
    int ran = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < HP_SCHED_CAP; i++) {
            if (!s->slot[i].used || before(now, s->slot[i].due) || (int32_t)(s->slot[i].seq - limit) >= 0) continue;
            if (best < 0 || before(s->slot[i].due, s->slot[best].due) ||
                (s->slot[i].due == s->slot[best].due && before(s->slot[i].seq, s->slot[best].seq)))
                best = i;
        }
        if (best < 0) return ran;
        hp_sched_cb cb = s->slot[best].cb;
        void *arg = s->slot[best].arg;
        s->slot[best].used = 0;
        cb(arg, now);
        ran++;
    }
}
