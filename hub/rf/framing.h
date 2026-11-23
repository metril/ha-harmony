#ifndef HP_RF_FRAMING_H
#define HP_RF_FRAMING_H

/* RF object layer over the CC2544 28-byte inner block (docs/hardware/hub-rf-cc2544.md s6).
 * Pure: no I/O, no threads, no clock of its own (the sender takes `now_ms` from the caller).
 *
 * Inner block: [0] hdr = kind | 0x10 (first frame of an object) | seq (low 4 bits)
 *              [1] type  [2] flags  [3..27] payload[25]
 * kind: 0x20 object frame, 0x40 ack, 0x60 continuation, 0x80 poll (all-zero otherwise: a remote's "I am here,
 * send me what you have" transmission; the receiver ignores it: no ack, no delivery, partials untouched).
 * Object = len(1) + body (body <= 254), carried in 25-byte chunks, zero padded. All frames
 * of an object share seq and type. `flags` is the frame index within the object (0 on the
 * first frame, where a receiver ignores it so captured stock frames still decode); without
 * it a duplicated continuation could not be told from the next one. No CRC: the radio
 * guarantees integrity and stock frames carry none.
 * Stop-and-wait: one ack (kind 0x40, same seq and type) per complete object. */

#include <stddef.h>
#include <stdint.h>
#include "proto/gen/harmony_proto.h"

#define HP_RF_INNER_LEN 28
#define HP_RF_CHUNK 25
#define HP_RF_BODY_MAX 254
#define HP_RF_MSG_MAX HP_RF_BODY_MAX
#define HP_RF_MAX_FRAGS 11 /* ceil((1 + 254) / 25) */
#define HP_RF_HUB_FRAME_LEN 30    /* 0x12, slot, inner[28]: what the hub's SPI carries */
#define HP_RF_REMOTE_FRAME_LEN 32 /* 0x20, pipe|power, 0x12, slot, inner[28]: the remote's SPI */
#define HP_RF_FRAME_MAX 32

#define HP_RF_KIND_OBJECT 0x20
#define HP_RF_KIND_ACK 0x40
#define HP_RF_KIND_CONT 0x60
#define HP_RF_KIND_POLL 0x80
#define HP_RF_FLAG_FIRST 0x10

#define HP_RF_RETRIES 3
#define HP_RF_RETRY_MS 150
#define HP_RF_BUSY_MS 50 /* a send refused with -HP_E_BUSY (transport backoff) is not a retry: tried again after this */

typedef struct {
    uint8_t b[HP_RF_INNER_LEN];
} hp_rf_inner_t;

/* An outer (on-the-wire) frame. */
typedef struct {
    uint8_t data[HP_RF_FRAME_MAX];
    size_t len;
} hp_rf_frame_t;

typedef struct {
    hp_msg_type_t type;
    uint8_t seq;
    size_t len;
    uint8_t payload[HP_RF_MSG_MAX];
} hp_rf_msg_t;

/* ---- encode ---- */

/* Splits one object into inner blocks. 0, -HP_E_TOOBIG (len > 254 or out_cap too small),
 * -HP_E_INVAL (bad args / type > 0xff). Never truncates; *out_n untouched on failure. */
int hp_rf_split(hp_msg_type_t t, uint8_t seq, const uint8_t *body, size_t len,
                hp_rf_inner_t *out, size_t out_cap, size_t *out_n);

void hp_rf_make_ack(uint8_t type, uint8_t seq, hp_rf_inner_t *out);
/* The poll frame: 0x80 followed by zeros. */
void hp_rf_make_poll(hp_rf_inner_t *out);

/* ---- outer wrapping (decoders reject wrong report ids / lengths with -HP_E_INVAL) ---- */

/* Hub mode, 30 bytes: 0x12, slot, inner. */
void hp_rf_wrap_hub(uint8_t slot, const hp_rf_inner_t *in, hp_rf_frame_t *out);
int hp_rf_unwrap_hub(const uint8_t *f, size_t len, uint8_t *slot, hp_rf_inner_t *in);

/* Remote mode, 32 bytes: 0x20, (slot & 0xf) | powermode, 0x12, slot, inner. */
void hp_rf_wrap_remote(uint8_t slot, uint8_t powermode, const hp_rf_inner_t *in, hp_rf_frame_t *out);
int hp_rf_unwrap_remote(const uint8_t *f, size_t len, uint8_t *slot, uint8_t *byte1, hp_rf_inner_t *in);

/* ---- receive: reassembly + duplicate suppression ---- */

#define HP_ASM_INCOMPLETE 0 /* frame consumed, object not done */
#define HP_ASM_COMPLETE 1   /* object delivered in *out; send *ack */
#define HP_ASM_DUPLICATE 2  /* complete retry of the last delivered object: send *ack, deliver nothing */
#define HP_ASM_ACK 3        /* an ack arrived: out->type/seq name the acked object */
#define HP_ASM_POLL 4       /* a poll arrived: nothing to do, reassembly state untouched */

typedef struct {
    int active;
    int have_last;
    uint8_t last_seq; /* last delivered seq and type (one assembler per peer direction) */
    uint8_t last_type;
    uint8_t type, seq, next_idx;
    uint8_t len; /* body length */
    size_t have; /* object bytes (length byte included) received so far */
    uint8_t buf[HP_RF_BODY_MAX + 1];
} hp_rf_asm_t;

/* Also forgets the last delivered seq: call on a link change. */
void hp_rf_asm_init(hp_rf_asm_t *a);

/* Feeds one inner block. Returns an HP_ASM_* code, or -HP_E_INVAL for a frame that does not
 * belong (bad kind, continuation without a matching partial, wrong frame index, bad length):
 * any partial is abandoned, nothing is delivered or acked. *out / *ack are written only for
 * the codes that say so. */
int hp_rf_asm_push(hp_rf_asm_t *a, const hp_rf_inner_t *in, hp_rf_msg_t *out, hp_rf_inner_t *ack);

/* ---- send: stop-and-wait state machine (injected clock and send callback) ---- */

typedef int (*hp_rf_send_fn)(void *ctx, const hp_rf_inner_t *frame);

typedef enum { HP_RF_TX_IDLE, HP_RF_TX_WAIT, HP_RF_TX_OK, HP_RF_TX_FAILED } hp_rf_tx_state_t;

typedef struct {
    hp_rf_send_fn send;
    void *ctx;
    hp_rf_tx_state_t state;
    uint8_t next_seq, seq, type;
    uint8_t retries; /* retransmissions done so far */
    uint8_t busy;    /* the last send attempt was refused with -HP_E_BUSY (not counted as a retry) */
    uint32_t deadline;
    size_t n;
    /* Pacing. pace_ms == 0 (default): every frame goes out at once. pace_ms > 0: frame 0 goes out at
     * start, each further frame only from hp_rf_tx_service(), after a TX status or `pace_ms`. */
    uint32_t pace_ms;
    uint32_t next_at; /* earliest time of the next paced frame */
    size_t sent;      /* frames of the current transmission already handed to send() */
    hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
} hp_rf_tx_t;

void hp_rf_tx_init(hp_rf_tx_t *tx, hp_rf_send_fn send, void *ctx);

/* Allocates the next seq, sends every frame, arms the retry timer. -HP_E_BUSY while an object
 * is in flight; -HP_E_TOOBIG/-HP_E_INVAL as hp_rf_split; -HP_E_IO if the send callback fails on
 * the first transmission (nothing is in flight then). Allowed from IDLE/OK/FAILED. */
int hp_rf_tx_start(hp_rf_tx_t *tx, hp_msg_type_t t, const uint8_t *body, size_t len, uint32_t now_ms);

/* Paced transmissions only: sends the next frame when `released` (the radio reported TX done for the
 * previous one) or the inter-frame gap expired. Returns 1 if a frame was sent. The retry timer is
 * (re)armed from the last frame. No-op unless a paced object is part-sent. */
int hp_rf_tx_service(hp_rf_tx_t *tx, uint32_t now_ms, int released);

/* Feed an ack seen by hp_rf_asm_push (HP_ASM_ACK). 1 if it completed the in-flight object. */
int hp_rf_tx_on_ack(hp_rf_tx_t *tx, uint8_t type, uint8_t seq);

/* Drives retries: resends the whole object every HP_RF_RETRY_MS, up to HP_RF_RETRIES times,
 * then FAILED. Returns the state. */
hp_rf_tx_state_t hp_rf_tx_poll(hp_rf_tx_t *tx, uint32_t now_ms);

#endif
