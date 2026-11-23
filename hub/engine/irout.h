#ifndef HP_ENGINE_IROUT_H
#define HP_ENGINE_IROUT_H

/* IR output runtime: sits between the engine and the IR sink and adds what a single encode can't.
 *
 *  - Sony SIRC (12/15/20): every send emits the frame HP_IROUT_SIRC_COPIES times, `period` apart.
 *    The first goes out immediately, the rest from timers (never sleeping).
 *  - RC5/RC6: the toggle bit flips on every new send per (protocol, address) and is constant for
 *    the repeats of one hold.
 *  - Hold/repeat: a send tagged with a key id may be repeated while the key is held. hold(key)
 *    starts a timer at the signal's period (HP_IROUT_DEFAULT_PERIOD_MS when 0) that sends the
 *    repeat sequence: the NEC repeat code, the Pronto repeat part, or the full frame for
 *    protocols without one. release(key) stops it. The remote sends a HOLD event every
 *    HP_IROUT_HOLD_INTERVAL_MS while the key is held; each HOLD (re)starts the
 *    HP_IROUT_HOLD_CAP_MS safety cap, so a real long hold keeps repeating and a lost RELEASE stops
 *    the repeats within the cap of the last HOLD. The first repeat is one period after the first
 *    HOLD. Repeat ticks are skipped while SIRC copies are still pending.
 * Line ownership: hp_irout owns a "line free at" time (frame + SIRC copies + HP_IROUT_GUARD_MS) pushed
 * by every send. A send arriving while the line is busy is deferred until it frees (small FIFO of
 * HP_IROUT_PEND, sent in order; when full the newest is dropped), never interleaved with pending copies; plan steps wait
 * (hp_irout_wait_ms). Hold repeats are skipped while the line is busy.
 * One hold at a time: a new send or another key's press ends the previous hold. */

#include <stdbool.h>
#include <stdint.h>

#include "hub/engine/sched.h"
#include "hub/io/sinks.h"

#define HP_IROUT_SIRC_COPIES 3
#define HP_IROUT_MAX_COPIES 8 /* the `repeats` limit of config format 2 */
#define HP_IROUT_DEFAULT_PERIOD_MS 110
#define HP_IROUT_HOLD_CAP_MS 2000
#define HP_IROUT_HOLD_INTERVAL_MS 400 /* remote -> hub HOLD event period while a key is held */
#define HP_IROUT_GUARD_MS 50 /* quiet time after a frame before the next one may start */
#define HP_IROUT_KEY_NONE (-1) /* hp_irout_send key: send now (plan/activity steps, which wait on the line) */
#define HP_IROUT_KEY_SEND (-2) /* ... an ir_send: deferred while the line is busy, no hold candidate */
#define HP_IROUT_PEND 16 /* deferred sends queued while the line is busy */
#define HP_IROUT_BURSTS 4
#define HP_IROUT_TOGGLES 8

typedef struct hp_irout hp_irout_t;

typedef struct {
    int used;
    uint8_t left, port;
    uint32_t period_ms;
    hp_sched_handle h;
    hp_irout_t *o;
    hp_ir_signal_t sig;
} hp_irout_burst_t;

typedef struct {
    uint8_t port;
    int key;
    bool sirc;
    uint8_t copies; /* frames in total (>= 1) */
    hp_ir_sink *ir;
    void (*done)(void *ctx, int rc); /* result of the deferred send (NULL = nobody waits) */
    void *dctx;
    hp_ir_signal_t sig;
} hp_irout_pend_t;

struct hp_irout {
    hp_sched_t *sched;
    hp_ir_sink *ir; /* sink of the latest send; used by timers */
    struct {
        uint8_t used, proto, t;
        uint32_t addr;
    } tog[HP_IROUT_TOGGLES];
    uint8_t tog_next;
    hp_irout_burst_t burst[HP_IROUT_BURSTS];
    int cand_key; /* key whose frame is stored in cand, -1 none */
    uint8_t cand_port;
    hp_ir_signal_t cand;
    int hold_key; /* -1 = not repeating */
    uint8_t hold_port;
    uint32_t hold_period_ms;
    hp_sched_handle tick_h, cap_h;
    hp_ir_signal_t rep;
    uint32_t busy_ms; /* how long the latest hp_irout_send occupies the IR line, SIRC copies included */
    /* Line-free time: every frame (press, hold repeat, SIRC copies, ir_send, plan step) pushes it.
     * Trusted only while it lies at most line_len ahead of now, so a stale value from a long idle
     * never looks like the future (32-bit ms clock). */
    uint32_t line_free, line_len;
    /* Presses / ir_sends that arrived while the line was busy: a small FIFO, sent in order as the
     * line frees. Full: the newest is dropped (logged). */
    hp_irout_pend_t pend[HP_IROUT_PEND];
    uint8_t pend_head, pend_n;
    hp_sched_handle pend_h;
};

/* Ms until the IR line is free for a new frame (0 = free now; >= 1 while a deferred send is
 * still queued). Plan steps wait this long instead of interleaving. */
uint32_t hp_irout_wait_ms(const hp_irout_t *, uint32_t now);
void hp_irout_init(hp_irout_t *, hp_sched_t *);
/* Encodes (flipping the toggle for RC5/RC6) and sends `code` on `port_mask`, ends any hold, and
 * schedules the extra SIRC copies. key >= 0 (a key press) makes it the hold candidate and, like
 * HP_IROUT_KEY_SEND, defers the send while the line is busy; HP_IROUT_KEY_NONE (-1) sends at once. Returns the encoder's or
 * sink's negative error; a failed first send schedules nothing. */
int hp_irout_send(hp_irout_t *, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key);
/* Same, sending the frame `copies` times in total (1..8, one frame period apart, like the SIRC copies; a SIRC
 * code is always sent at least HP_IROUT_SIRC_COPIES times). The per-command `repeats` of config format 2. */
int hp_irout_send_n(hp_irout_t *, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key, uint8_t copies);
/* hp_irout_send_n with a completion callback for a DEFERRED send: returns 0 when the frame went out
 * now (done is not called), 1 when it was queued (done(ctx, rc) is called exactly once when it is
 * finally sent, rc = the send result), or a negative error (done not called). Distinct presses are never
 * merged; hold repeats are already skipped while a send is queued. */
int hp_irout_send_cb(hp_irout_t *, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key, uint8_t copies,
                     void (*done)(void *ctx, int rc), void *ctx);
/* Starts repeating the last frame sent for `key`; if `key` is already repeating this only extends
 * the safety cap (no-op when `key` has no candidate frame). */
void hp_irout_hold(hp_irout_t *, int key);
/* Stops the hold of `key` (key < 0: any). */
void hp_irout_release(hp_irout_t *, int key);
/* Ends any hold and forgets the candidate (a new, non-IR press). */
void hp_irout_cancel(hp_irout_t *);

#endif
