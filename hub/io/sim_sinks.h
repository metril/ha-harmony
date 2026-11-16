#ifndef HP_IO_SIM_SINKS_H
#define HP_IO_SIM_SINKS_H

/* Host simulation backends: recording IR/BT sinks and a scripted RF transport.
 *
 * Every sink call is appended to an in-memory ring (the last HP_SIM_RING
 * calls; hp_sim_count() is the total ever recorded, hp_sim_rec(i) addresses
 * by absolute index while still in the ring) and, when a mirror FILE is
 * given, printed as one line and flushed. Line formats (stable; parsed by
 * Python tests):
 *
 *   ir port=<port_mask decimal> hz=<carrier> n=<duration count> us=<d0>,<d1>,...
 *       durations are decimal microseconds (the once-part of the signal only)
 *   bt usage=<4-digit lowercase hex> down=<0|1>
 *
 * Example:  ir port=3 hz=38029 n=2 us=631,631
 *           bt usage=00e9 down=1
 */

#include <stdio.h>

#include "hub/io/sinks.h"

#define HP_SIM_RING 64
#define HP_SIM_MAX_US 128 /* durations kept per ring record (the mirror line is never cut) */
#define HP_SIM_RF_QUEUE 16
#define HP_SIM_RF_MAX 64

typedef enum { HP_SIM_IR = 1, HP_SIM_BT = 2 } hp_sim_kind_t;

typedef struct {
    hp_sim_kind_t kind;
    uint8_t port_mask;
    uint32_t hz;
    size_t n;                   /* durations sent */
    uint16_t us[HP_SIM_MAX_US]; /* first min(n, HP_SIM_MAX_US) durations */
    uint16_t usage;
    int down;
} hp_sim_rec_t;

typedef struct {
    hp_ir_sink ir;
    hp_bt_sink bt;
    hp_rf_transport rf;
    FILE *mirror;
    size_t total;
    hp_sim_rec_t ring[HP_SIM_RING];
    bool rf_claimed;
    uint8_t rfq[HP_SIM_RF_QUEUE][HP_SIM_RF_MAX];
    size_t rfq_len[HP_SIM_RF_QUEUE];
    size_t rfq_head, rfq_count;
} hp_sim_t;

/* mirror may be NULL (record only). */
void hp_sim_init(hp_sim_t *, FILE *mirror);
hp_sinks_t hp_sim_sinks(hp_sim_t *);
/* Total calls recorded since init. */
size_t hp_sim_count(const hp_sim_t *);
/* Record by absolute index (0 = first call); NULL if evicted or out of range. */
const hp_sim_rec_t *hp_sim_rec(const hp_sim_t *, size_t index);
/* Queues a frame the scripted RF transport will return from recv().
 * -HP_E_TOOBIG if len > HP_SIM_RF_MAX, -HP_E_BUSY if the queue is full. */
int hp_sim_rf_script(hp_sim_t *, const uint8_t *frame, size_t len);

#endif
