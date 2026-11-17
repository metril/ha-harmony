#ifndef HP_RF_LINK_H
#define HP_RF_LINK_H

/* Object link: the reliable layer over any hp_rf_transport, shared by hub and remote.
 * One hp_rf_tx_t sender (stop-and-wait, one ack per object, 3 retries at 150 ms) and one
 * reassembler (duplicate suppression). No threads, no real time, no allocation: the caller
 * passes `now_ms` to every call and supplies the transport, so it is deterministic under test.
 *
 * Frames on the transport are hub-mode 30-byte frames (0x12, slot, inner[28]) when `wrapped`
 * (the sim socket), else bare 28-byte inner blocks (hub/io/rf_spi.c wraps them itself).
 * Sending is queued (HP_LINK_QUEUE objects): hp_rf_link_send returns once the object is queued
 * and the pump delivers it. A object that exhausts its retries is dropped and counted in
 * `failures`, the "send failed" status. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub/io/sinks.h"
#include "hub/rf/framing.h"

#define HP_LINK_QUEUE 8
#define HP_LINK_GAP_MS 20     /* default minimum gap between the frames of one object */
#define HP_LINK_SLOT_ANY 0xFF /* tx_slot: accept a TX status for any slot */

typedef void (*hp_rf_link_deliver_fn)(void *ctx, const hp_rf_msg_t *msg);

typedef struct {
    hp_msg_type_t type;
    size_t len;
    uint8_t body[HP_RF_BODY_MAX];
} hp_rf_link_obj_t;

typedef void (*hp_rf_link_log_fn)(void *ctx, const char *line);

/* Default log sink: the line plus a newline on stderr. */
void hp_rf_log_stderr(void *ctx, const char *line);

typedef struct {
    hp_rf_transport *tp;
    bool wrapped;
    uint8_t slot;
    hp_rf_link_deliver_fn deliver;
    void *deliver_ctx;
    hp_rf_tx_t tx;
    hp_rf_asm_t as;
    hp_rf_link_obj_t q[HP_LINK_QUEUE];
    size_t qhead, qn;
    bool started;          /* q[qhead] is handed to tx (in flight, or awaiting a first transmission) */
    uint8_t start_tries;   /* first-transmission attempts for the head object */
    uint32_t start_at;     /* next attempt time while the transport refuses the first frame */
    int last_rc;           /* last transport send() result */
    bool noent;            /* the head object hit -HP_E_NOENT on its first frame and was dropped */
    uint32_t failures;     /* objects dropped after all retries (monotonic) */
    uint32_t delivered;    /* objects delivered up (monotonic) */
    /* Frame pacing (objects of 2+ frames): frame N+1 follows a TX status for tx_slot, or gap_ms. */
    uint32_t gap_ms;       /* HP_LINK_GAP_MS; set after init to change (0 = no pacing) */
    uint8_t tx_slot;       /* HP_LINK_SLOT_ANY by default */
    bool released;         /* a TX status arrived since the last data frame was sent */
    hp_rf_link_log_fn log; /* NULL = off (per-object debug events) */
    void *log_ctx;
    uint32_t now_dbg;      /* last caller clock, for log lines only */
} hp_rf_link_t;

typedef struct {
    size_t queued;     /* objects waiting or in flight */
    uint32_t failures; /* monotonic count of objects abandoned after retries */
    uint32_t delivered;
} hp_rf_link_status_t;

/* `tp` must already be claimed by the caller. deliver may be NULL (objects dropped). */
void hp_rf_link_init(hp_rf_link_t *l, hp_rf_transport *tp, bool wrapped, uint8_t slot,
                     hp_rf_link_deliver_fn deliver, void *ctx);

/* Enables per-object debug logging (send/frame/ack/retry/fail/deliver); fn NULL turns it off. */
void hp_rf_link_set_log(hp_rf_link_t *l, hp_rf_link_log_fn fn, void *ctx);

/* Queues one object (and transmits it at once when nothing is ahead of it). 0; -HP_E_TOOBIG (body > HP_RF_BODY_MAX); -HP_E_INVAL; -HP_E_NOENT when the
 * transport reports no peer, by attached() or by refusing the first frame (nothing queued); -HP_E_BUSY when the queue is full.
 * Retries and queued objects are driven by hp_rf_link_pump. */
int hp_rf_link_send(hp_rf_link_t *l, hp_msg_type_t type, const uint8_t *body, size_t len, uint32_t now_ms);

/* Processes every pending received frame (COMPLETE: deliver + ack; DUPLICATE: ack only; ACK: feed
 * the sender) and drives retries / the next queued object. Never blocks. Returns 0, or the
 * transport's negative error: -HP_E_NOENT once when the peer went away (reassembly reset, queued
 * and in-flight objects dropped and counted as failures), -HP_E_IO when the link is dead. */
int hp_rf_link_pump(hp_rf_link_t *l, uint32_t now_ms);

/* ms until the link wants a pump for a retry (0 = now), -1 when idle. */
int hp_rf_link_next_timeout_ms(const hp_rf_link_t *l, uint32_t now_ms);

hp_rf_link_status_t hp_rf_link_status(const hp_rf_link_t *l);

#endif
