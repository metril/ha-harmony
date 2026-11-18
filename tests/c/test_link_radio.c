/* Hub <-> remote over a fake radio that models the measured CC2544 behaviour (bench session 2):
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
static hp_rf_msg_t pages[8];
static int npages, ntiles;

static uint32_t clk(void *c) { (void)c; return R.now; }

static void rem_deliver(void *ctx, const hp_rf_msg_t *m) {
    (void)ctx;
    if (m->type == HP_MSG_PAGE_PUSH && npages < 8) pages[npages++] = *m;
    if (m->type == HP_MSG_TILE_UPDATE) ntiles++;
}

static void world_init(bool poll, model_t model) {
    memset(&R, 0, sizeof R);
    R.model = model;
    R.now = 1000;
    R.hub_t = (hp_rf_transport){NULL, hub_send, hub_recv, claim, hub_attached, hub_txs, hub_times};
    R.rem_t = (hp_rf_transport){NULL, rem_send, rem_recv, claim, NULL, NULL, NULL};
    npages = ntiles = 0;
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

static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
        R.now += 5;
        radio_tick();
        (void)hp_rf_link_pump(&RL, R.now);
        hub_pump_rf(&H);
    }
}

/* page_push through the real handler; 1 = replied command_result ok:true */
static int push_page(uint8_t page, const char *b64) {
    char js[160];
    snprintf(js, sizeof js, "{\"remote\":1,\"page\":%u,\"data\":\"%s\"}", (unsigned)page, b64);
    cJSON *d = cJSON_Parse(js);
    char reply[512];
    size_t len = 0;
    int rc = TBL.handler[HP_MSG_PAGE_PUSH](&H, NULL, 7, d, reply, sizeof reply, &len);
    cJSON_Delete(d);
    reply[len < sizeof reply ? len : sizeof reply - 1] = 0;
    return rc == 0 && strstr(reply, "command_result") && strstr(reply, "\"ok\":true");
}

static int send_uplink(void) { /* the stand-in for a key press: any object the hub must ack */
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 80}, {.u = 0}};
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    if (hp_rf_pack(HP_MSG_BATTERY, v, 3, pl, sizeof pl, &n) < 0) return -1;
    return hp_rf_link_send(&RL, HP_MSG_BATTERY, pl, n, R.now);
}

/* 56 chars = 42 bytes: page_push body of 3 + 42 + header spans two RF frames */
#define B64_A "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFB"
#define B64_B "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJC"

static void test_uplink_acked_without_retries_with_polling(model_t m) {
    world_init(true, m);
    run(1000); /* polls bring the link up */
    CT_CHECK(R.up);
    for (int i = 0; i < 5; i++) { /* five "key presses", one after another */
        int before = R.remote_objs_sent;
        CT_EQ(send_uplink(), 0);
        run(120); /* well under the 150 ms retry */
        CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* acked */
        CT_EQ(R.remote_objs_sent - before, 1);     /* ... on the first transmission */
    }
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 5u);
    CT_EQ(R.hub_acks_written, 5);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_without_polling_the_ack_waits_for_the_retry(void) { /* S2 only */
    world_init(false, MODEL_S2);
    CT_EQ(send_uplink(), 0);
    run(100);
    CT_EQ(hp_rf_link_status(&RL).queued, 1u); /* the ack is stuck in the radio until the remote speaks again */
    run(200);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* the retry carried it */
}

static void test_two_frame_page_delivered_after_link_up(model_t m) {
    world_init(true, m);
    run(400);
    CT_CHECK(R.up);
    CT_CHECK(push_page(3, B64_A));
    run(600);
    CT_EQ(npages, 1);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u); /* and acked */
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
    CT_EQ(R.hub_data_written, 2); /* two frames, no retry */
    CT_EQ(pages[0].payload[pages[0].len - 1], 'A');
    CT_CHECK(R.max_depth <= 1);
}

static void test_page_pushed_while_link_down_is_sent_on_link_up(model_t m) {
    world_init(true, m);
    run(400);
    hp_rf_link_set_awake(&RL, false, R.now); /* remote goes quiet: link lost after 500 ms */
    run(800);
    CT_CHECK(!R.up);
    CT_CHECK(push_page(3, B64_A)); /* plain ok, held */
    CT_CHECK(push_page(3, B64_B)); /* the latest per (remote,page) wins */
    CT_EQ(H.npend, 1);
    run(300);
    CT_EQ(npages, 0);
    CT_EQ(R.hub_data_written, 0); /* nothing written to a dead link */
    hp_rf_link_set_awake(&RL, true, R.now); /* the remote wakes and polls */
    run(800);
    CT_CHECK(R.up);
    CT_EQ(H.npend, 0);
    CT_EQ(npages, 1);
    CT_EQ(pages[0].payload[pages[0].len - 1], 'B');
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(R.hub_writes_while_down, 0);
    CT_CHECK(R.max_depth <= 1);
}

static void test_no_duplicate_ack_pile_up(model_t m) {
    world_init(true, m);
    run(400);
    R.drop_dl = 3; /* the first acks are lost on the air: the remote retries the object */
    CT_EQ(send_uplink(), 0);
    run(900);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 1u); /* retries are duplicates, not redeliveries */
    CT_CHECK(R.max_depth <= 1);
    /* at most one ack per transmission of the object the hub saw, never a burst of stale ones */
    CT_CHECK(R.hub_acks_written <= R.remote_objs_sent);
    run(2000);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_link_flap_keeps_queue_and_sends_in_order(model_t m) {
    world_init(true, m);
    run(400);
    CT_CHECK(push_page(1, B64_A));
    hp_rf_link_set_awake(&RL, false, R.now); /* link drops mid-delivery */
    run(40);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(1500);
    CT_EQ(npages, 1);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_CHECK(R.max_depth <= 1);
}

/* ---- S3 only: the radio delivers by itself, so only an uplink frame proves delivery ---- */

static void test_s3_key_pair_acked_first_try(void) {
    world_init(true, MODEL_S3);
    run(1000);
    CT_CHECK(R.up);
    for (int i = 0; i < 2; i++) { /* the second object goes right after the first is acked */
        CT_EQ(send_uplink(), 0);
        uint32_t t0 = R.now;
        while (hp_rf_link_status(&RL).queued && R.now - t0 < 300) run(5);
        CT_CHECK(R.now - t0 <= 100);
        CT_EQ(R.remote_objs_sent, i + 1); /* first try */
    }
    CT_EQ(R.hub_acks_written, 2);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_CHECK(R.max_depth <= 1);
    CT_CHECK(R.max_lat <= 15);
}

static void test_s3_two_frame_page_fast(void) {
    world_init(true, MODEL_S3);
    run(400);
    CT_CHECK(push_page(3, B64_A));
    uint32_t t0 = R.now;
    while ((npages < 1 || hp_rf_link_status(&H.link).queued) && R.now - t0 < 600) run(5);
    CT_EQ(npages, 1);
    CT_CHECK(R.now - t0 <= 120);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
}

static void test_s3_asleep_remote_falls_back_to_timeout(void) {
    world_init(true, MODEL_S3);
    run(400);
    while (R.now - R.last_uplink >= 5) run(5); /* just after a poll: the link stays up for 500 ms more */
    hp_rf_link_set_awake(&RL, false, R.now);   /* no more polls */
    CT_CHECK(R.up);
    CT_CHECK(push_page(3, B64_A));
    run(400);
    CT_EQ(npages, 1); /* frame 2 left on the 300 ms timeout */
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_s3_dropped_downlink_dedup(void) {
    world_init(true, MODEL_S3);
    run(400);
    R.drop_dl = 2;
    CT_EQ(send_uplink(), 0);
    run(1500);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 1u);
    CT_CHECK(R.hub_acks_written <= R.remote_objs_sent);
    CT_CHECK(R.max_depth <= 1);
}

/* An uplink that arrived BEFORE the write but is read after it (late reader) must not release the frame. */
static void test_late_read_of_pre_write_uplink_does_not_release(model_t m) {
    world_init(true, m);
    run(1000);
    R.hold_rx = true; /* the hub's reader is behind */
    int polls = R.remote_polls_sent;
    while (R.remote_polls_sent == polls) run(5); /* a poll arrives, unread */
    CT_CHECK(push_page(3, B64_A));
    run(5); /* the hub writes frame 1 */
    CT_EQ(R.hub_data_written, 1);
    R.hold_rx = false;
    run(5); /* the stale uplink is read 5 ms after the write */
    CT_EQ(R.hub_data_written, 1);
    CT_CHECK(R.max_depth <= 1);
    run(1500);
    CT_EQ(npages, 1);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
}

/* An uplink arriving within the margin after the write does not release; one after the margin does. */
static void test_uplink_margin(model_t m) {
    for (int late = 0; late < 2; late++) {
        world_init(true, m);
        run(1000);
        CT_CHECK(push_page(3, B64_A));
        run(5); /* frame 1 written at R.now */
        CT_EQ(R.hub_data_written, 1);
        uint32_t t_w = R.now;
        if (late) run(UPLINK_PREP_MS); /* arrives exactly one margin after the write */
        CT_EQ(send_uplink(), 0);
        (void)t_w;
        run(5); /* the hub reads it */
        CT_EQ(R.hub_acks_written, late ? 1 : 0); /* the uplink's ack goes out at once only if it released the slot */
        CT_EQ(R.hub_data_written, 1);
        CT_CHECK(R.max_depth <= 1);
        run(1500);
        CT_EQ(npages, 1);
        CT_CHECK(R.max_depth <= 1);
    }
}

/* A transport without frame_times: no uplink release, the 300 ms timeout (or a status 00) only. */
static void test_without_stamps_release_is_by_timeout(void) {
    world_init(true, MODEL_S3);
    R.hub_t.frame_times = NULL;
    run(400);
    CT_CHECK(push_page(3, B64_A));
    uint32_t t0 = R.now;
    while (npages < 1 && R.now - t0 < 800) run(5);
    CT_EQ(npages, 1);
    CT_CHECK(R.now - t0 >= (uint32_t)HP_LINK_OUT_TIMEOUT_MS);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
}

int main(void) {
    for (model_t m = MODEL_S2; m <= MODEL_S3; m++) {
        test_uplink_acked_without_retries_with_polling(m);
        test_two_frame_page_delivered_after_link_up(m);
        test_page_pushed_while_link_down_is_sent_on_link_up(m);
        test_no_duplicate_ack_pile_up(m);
        test_link_flap_keeps_queue_and_sends_in_order(m);
        test_late_read_of_pre_write_uplink_does_not_release(m);
        test_uplink_margin(m);
    }
    test_without_polling_the_ack_waits_for_the_retry();
    test_without_stamps_release_is_by_timeout();
    test_s3_key_pair_acked_first_try();
    test_s3_two_frame_page_fast();
    test_s3_asleep_remote_falls_back_to_timeout();
    test_s3_dropped_downlink_dedup();
    return CT_DONE();
}
