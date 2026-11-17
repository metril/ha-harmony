/* Hub <-> remote over a fake radio that models the measured CC2544 behaviour (bench session 2):
 *  - hub->remote frames are held in the radio (one slot) and delivered only piggybacked on the remote's
 *    next uplink transmission (a poll counts);
 *  - the hub link goes up when the remote transmits and is lost 500 ms after the last uplink;
 *  - TX status: 00 when the held frame was delivered, 01 when the link dropped with it pending; a stale
 *    00 also arrives at link-up with nothing held.
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

typedef struct {
    uint32_t now;
    bool up, lost_pending;
    uint32_t last_uplink;
    bool dl_v;          /* a hub->remote frame is held in the radio */
    uint8_t dl[HP_RF_INNER_LEN];
    int max_depth, depth_now;
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
    if (R.dl_v) R.max_depth = R.max_depth < 2 ? 2 : R.max_depth; /* a second frame while one is held */
    memcpy(R.dl, f, n);
    R.dl_v = true;
    R.depth_now = R.dl_v ? 1 : 0;
    if (R.max_depth < 1) R.max_depth = 1;
    return 0;
}

static int hub_recv(void *self, uint8_t *f, size_t cap, int ms) {
    (void)self; (void)ms; (void)cap;
    if (R.hrx_n) {
        memcpy(f, R.hrx[R.hrx_h], HP_RF_INNER_LEN);
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
    if (R.hrx_n < QN) memcpy(R.hrx[(R.hrx_h + R.hrx_n++) % QN], f, n);
    if (R.dl_v) { /* the hub's frame rides on this transmission */
        R.dl_v = false;
        R.depth_now = 0;
        if (R.drop_dl > 0) R.drop_dl--;
        else if (R.rrx_n < QN) memcpy(R.rrx[(R.rrx_h + R.rrx_n++) % QN], R.dl, HP_RF_INNER_LEN);
        status(0x00);
    }
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
    if (R.up && R.now - R.last_uplink >= IDLE_DROP_MS) {
        R.up = false;
        R.lost_pending = true;
        if (R.dl_v) { /* dropped with a frame pending */
            R.dl_v = false;
            R.depth_now = 0;
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

static void world_init(bool poll) {
    memset(&R, 0, sizeof R);
    R.now = 1000;
    R.hub_t = (hp_rf_transport){NULL, hub_send, hub_recv, claim, hub_attached, hub_txs};
    R.rem_t = (hp_rf_transport){NULL, rem_send, rem_recv, claim, NULL, NULL};
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

static void test_uplink_acked_without_retries_with_polling(void) {
    world_init(true);
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

static void test_without_polling_the_ack_waits_for_the_retry(void) {
    world_init(false);
    CT_EQ(send_uplink(), 0);
    run(100);
    CT_EQ(hp_rf_link_status(&RL).queued, 1u); /* the ack is stuck in the radio until the remote speaks again */
    run(200);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* the retry carried it */
}

static void test_two_frame_page_delivered_after_link_up(void) {
    world_init(true);
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

static void test_page_pushed_while_link_down_is_sent_on_link_up(void) {
    world_init(true);
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

static void test_no_duplicate_ack_pile_up(void) {
    world_init(true);
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

static void test_link_flap_keeps_queue_and_sends_in_order(void) {
    world_init(true);
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

int main(void) {
    test_uplink_acked_without_retries_with_polling();
    test_without_polling_the_ack_waits_for_the_retry();
    test_two_frame_page_delivered_after_link_up();
    test_page_pushed_while_link_down_is_sent_on_link_up();
    test_no_duplicate_ack_pile_up();
    test_link_flap_keeps_queue_and_sends_in_order();
    return CT_DONE();
}
