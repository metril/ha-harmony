#ifndef FAKE_RADIO_H
#define FAKE_RADIO_H
/* Shared by test_link_radio.c and test_link_v2.c. Hub <-> remote over a fake radio that models the measured CC2544 behaviour (bench session 2):
 *  - hub->remote frames are held in the radio (one slot) and delivered only piggybacked on the remote's
 *    next uplink transmission (a poll counts);
 *  - the hub link goes up when the remote transmits and is lost 500 ms after the last uplink;
 *  - TX status: 00 when the held frame was delivered, 01 when the link dropped with it pending; a stale
 *    00 also arrives at link-up with nothing held.
 * The radio buffer is a FIFO (a second write queues, so depth 2 is observable). An uplink delivers the head
 * frame only if it starts >= UPLINK_PREP_MS after that frame's write (a frame already in flight at the write
 * does not carry it): this is what the hub's 5 ms release margin assumes. The fake stamps arrival (uplink
 * reaching the hub) and write times like rf_spi's frame_times hook and can delay the hub's reads (hold_rx).
 * Two models (MODEL_S2 = the above; MODEL_S3 = bench session 3): in S3 a held frame is also delivered 8 ms
 * after the write by the radio itself while the link is up (no host uplink needed), and delivery emits NO
 * status 00 (only the stale one at link-up and 01 on a drop). An uplink still delivers a held frame.
 * The hub end is the real hub context (page_push handler, held-message store, strict downlink link),
 * the remote end the real hp_rf_link_t with polling. Deterministic: fake clock, no threads. */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/rf/link.h"
#include "hub/rf/rf_payload.h"

#define QN 64
#define IDLE_DROP_MS 500
#define S3_DELIVER_MS 8
#define UPLINK_PREP_MS 5
#define DLQ 8

typedef enum { MODEL_S2, MODEL_S3 } model_t;

typedef struct {
    uint32_t now;
    bool up, lost_pending;
    uint32_t last_uplink;
    model_t model;
    uint32_t max_lat;   /* max write->remote delivery latency (ms) */
    uint8_t dl[DLQ][HP_RF_INNER_LEN]; /* hub->remote frames held in the radio, oldest first */
    uint32_t dl_at[DLQ];
    int dl_n;
    uint32_t last_wr, last_rx_at, hrx_at[QN]; /* stamps: last write finish, arrival of the last frame read, per queued uplink */
    bool hold_rx;       /* the hub's reader has not got to the queued uplinks yet */
    int max_depth;
    int hub_acks_written, hub_data_written, hub_writes_while_down;
    int remote_objs_sent, remote_polls_sent;
    int drop_dl;        /* delivered frames to lose on the air (status still 00) */
    uint8_t hrx[QN][HP_RF_INNER_LEN];
    int hrx_h, hrx_n;
    uint8_t rrx[QN][HP_RF_INNER_LEN];
    int rrx_h, rrx_n;
    uint8_t txs[QN];
    int txs_h, txs_n;
    hp_rf_transport hub_t, rem_t;
} radio_t;

static radio_t R;

static void status(uint8_t s) {
    if (R.txs_n < QN) R.txs[(R.txs_h + R.txs_n++) % QN] = s;
}

static int hub_send(void *self, const uint8_t *f, size_t n) {
    (void)self;
    if (n != HP_RF_INNER_LEN) return -HP_E_INVAL;
    if ((f[0] & 0xE0) == HP_RF_KIND_ACK) R.hub_acks_written++;
    else R.hub_data_written++;
    if (!R.up) { /* a write with no link: the radio gives up on it at once */
        R.hub_writes_while_down++;
        status(0x01);
        return 0;
    }
    if (R.dl_n == DLQ) return -HP_E_BUSY;
    memcpy(R.dl[R.dl_n], f, n);
    R.dl_at[R.dl_n++] = R.now;
    R.last_wr = R.now;
    if (R.dl_n > R.max_depth) R.max_depth = R.dl_n;
    return 0;
}

static int hub_recv(void *self, uint8_t *f, size_t cap, int ms) {
    (void)self; (void)ms; (void)cap;
    if (R.hrx_n && !R.hold_rx) {
        memcpy(f, R.hrx[R.hrx_h], HP_RF_INNER_LEN);
        R.last_rx_at = R.hrx_at[R.hrx_h];
        R.hrx_h = (R.hrx_h + 1) % QN;
        R.hrx_n--;
        return HP_RF_INNER_LEN;
    }
    if (R.lost_pending) {
        R.lost_pending = false;
        return -HP_E_NOENT;
    }
    return 0;
}
static int claim(void *self) { (void)self; return 0; }
static bool hub_attached(void *self) { (void)self; return R.up; }
static int hub_txs(void *self, uint8_t *slot, uint8_t *st) {
    (void)self;
    if (!R.txs_n) return 0;
    *slot = 1;
    *st = R.txs[R.txs_h];
    R.txs_h = (R.txs_h + 1) % QN;
    R.txs_n--;
    return 1;
}

/* The held frame reaches the remote (or is lost on the air). */
static void deliver_dl(bool with_status) {
    if (R.now - R.dl_at[0] > R.max_lat) R.max_lat = R.now - R.dl_at[0];
    if (R.drop_dl > 0) R.drop_dl--;
    else if (R.rrx_n < QN) memcpy(R.rrx[(R.rrx_h + R.rrx_n++) % QN], R.dl[0], HP_RF_INNER_LEN);
    memmove(R.dl[0], R.dl[1], (size_t)(DLQ - 1) * HP_RF_INNER_LEN);
    memmove(&R.dl_at[0], &R.dl_at[1], (size_t)(DLQ - 1) * sizeof R.dl_at[0]);
    R.dl_n--;
    if (with_status) status(0x00);
}

static int hub_times(void *self, uint32_t *rx_at, uint32_t *tx_at) {
    (void)self;
    *rx_at = R.last_rx_at;
    *tx_at = R.last_wr;
    return 1;
}

static int rem_send(void *self, const uint8_t *f, size_t n) {
    (void)self;
    if (n != HP_RF_INNER_LEN) return -HP_E_INVAL;
    if ((f[0] & 0xE0) == HP_RF_KIND_POLL) R.remote_polls_sent++;
    else if ((f[0] & 0xE0) != HP_RF_KIND_ACK) R.remote_objs_sent++;
    if (!R.up) { /* first uplink of a new link */
        R.up = true;
        status(0x00); /* the stale status seen at link-up */
    }
    R.last_uplink = R.now;
    if (R.hrx_n < QN) {
        R.hrx_at[(R.hrx_h + R.hrx_n) % QN] = R.now;
        memcpy(R.hrx[(R.hrx_h + R.hrx_n++) % QN], f, n);
    }
    if (R.dl_n && R.now - R.dl_at[0] >= UPLINK_PREP_MS) deliver_dl(R.model == MODEL_S2); /* the hub's frame rides on this transmission */
    return 0;
}
static int rem_recv(void *self, uint8_t *f, size_t cap, int ms) {
    (void)self; (void)ms; (void)cap;
    if (!R.rrx_n) return 0;
    memcpy(f, R.rrx[R.rrx_h], HP_RF_INNER_LEN);
    R.rrx_h = (R.rrx_h + 1) % QN;
    R.rrx_n--;
    return HP_RF_INNER_LEN;
}

static void radio_tick(void) {
    while (R.model == MODEL_S3 && R.up && R.dl_n && R.now - R.dl_at[0] >= S3_DELIVER_MS) deliver_dl(false);
    if (R.up && R.now - R.last_uplink >= IDLE_DROP_MS) {
        R.up = false;
        R.lost_pending = true;
        while (R.dl_n) { /* dropped with frames pending */
            R.dl_n--;
            status(0x01);
        }
    }
}

/* ---- the two ends ---- */

static hub_ctx_t H;
static hp_handler_tbl_t TBL;
static hp_rf_link_t RL;
static uint32_t clk(void *c) { (void)c; return R.now; }

/* Every object the remote end's link delivers goes here (a test sets it before world_init). */
static void (*remote_hook)(const hp_rf_msg_t *);
static void (*world_reset_hook)(void); /* called at the start of every world_init */
static void rem_deliver(void *ctx, const hp_rf_msg_t *m) {
    (void)ctx;
    if (remote_hook) remote_hook(m);
}

static __attribute__((unused)) void world_init(bool poll, model_t model) {
    memset(&R, 0, sizeof R);
    R.model = model;
    R.now = 1000;
    R.hub_t = (hp_rf_transport){NULL, hub_send, hub_recv, claim, hub_attached, hub_txs, hub_times};
    R.rem_t = (hp_rf_transport){NULL, rem_send, rem_recv, claim, NULL, NULL, NULL};
    if (world_reset_hook) world_reset_hook();
    hub_ctx_init(&H);
    hub_set_clock(&H, clk, NULL);
    H.sinks.rf = &R.hub_t;
    H.rf_inner_blocks = true;
    memset(&TBL, 0, sizeof TBL);
    hub_install(&TBL, &H);
    hp_rf_link_init(&RL, &R.rem_t, false, 0, rem_deliver, NULL);
    if (poll) {
        hp_rf_link_set_poll(&RL, HP_LINK_POLL_MS, HP_LINK_IDLE_POLL_MS);
        hp_rf_link_set_awake(&RL, true, R.now);
    }
}

static __attribute__((unused)) void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
        R.now += 5;
        radio_tick();
        (void)hp_rf_link_pump(&RL, R.now);
        hub_pump_rf(&H);
    }
}

/* page_push through the real handler; 1 = replied command_result ok:true */
static __attribute__((unused)) int push_page(uint8_t page, const char *b64) {
    char js[480];
    snprintf(js, sizeof js, "{\"remote\":1,\"page\":%u,\"data\":\"%s\"}", (unsigned)page, b64);
    cJSON *d = cJSON_Parse(js);
    char reply[512];
    size_t len = 0;
    int rc = TBL.handler[HP_MSG_PAGE_PUSH](&H, NULL, 7, d, reply, sizeof reply, &len);
    cJSON_Delete(d);
    reply[len < sizeof reply ? len : sizeof reply - 1] = 0;
    return rc == 0 && strstr(reply, "command_result") && strstr(reply, "\"ok\":true");
}

static __attribute__((unused)) int send_uplink(void) { /* the stand-in for a key press: any object the hub must ack */
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 80}, {.u = 0}};
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    if (hp_rf_pack(HP_MSG_BATTERY, v, 3, pl, sizeof pl, &n) < 0) return -1;
    return hp_rf_link_send(&RL, HP_MSG_BATTERY, pl, n, R.now);
}

#endif
