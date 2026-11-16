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
 * One hold at a time: a new send or another key's press ends the previous hold. */

#include <stdint.h>

#include "hub/engine/sched.h"
#include "hub/io/sinks.h"

#define HP_IROUT_SIRC_COPIES 3
#define HP_IROUT_DEFAULT_PERIOD_MS 110
#define HP_IROUT_HOLD_CAP_MS 2000
#define HP_IROUT_HOLD_INTERVAL_MS 400 /* remote -> hub HOLD event period while a key is held */
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
};

void hp_irout_init(hp_irout_t *, hp_sched_t *);
/* Encodes (flipping the toggle for RC5/RC6) and sends `code` on `port_mask`, ends any hold, and
 * schedules the extra SIRC copies. key >= 0 makes it the hold candidate. Returns the encoder's or
 * sink's negative error; a failed first send schedules nothing. */
int hp_irout_send(hp_irout_t *, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key);
/* Starts repeating the last frame sent for `key`; if `key` is already repeating this only extends
 * the safety cap (no-op when `key` has no candidate frame). */
void hp_irout_hold(hp_irout_t *, int key);
/* Stops the hold of `key` (key < 0: any). */
void hp_irout_release(hp_irout_t *, int key);
/* Ends any hold and forgets the candidate (a new, non-IR press). */
void hp_irout_cancel(hp_irout_t *);

#endif
