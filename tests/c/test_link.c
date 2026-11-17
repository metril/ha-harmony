/* Object link (hub/rf/link.c): two endpoints over an in-memory lossy transport with an injected
 * clock. Fully deterministic: no threads, no sleeps, no sockets. */
#include <stdio.h>
#include <string.h>

#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/rf/link.h"

#define QCAP 128

typedef struct {
    uint8_t f[HP_RF_FRAME_MAX];
    size_t len;
} wire_t;

typedef struct net net_t;
typedef struct {
    hp_rf_transport t;
    net_t *net;
    int idx;
    wire_t q[QCAP];
    int head, n;
    int drop_acks, drop_objs, drop_all, dup_next; /* applied to frames this endpoint SENDS */
    int sent_objs, sent_acks;                     /* frames that left this endpoint */
    int gone;                                     /* recv reports -HP_E_NOENT once */
    int refuse;                                   /* send fails with -HP_E_IO this many times */
    uint32_t at[64];                              /* net->now of each non-ack frame sent */
    int nat;
    uint8_t txs[16][2];                           /* queued TX statuses (slot, status) */
    int txs_n;
    hp_rf_link_t link;
    hp_rf_msg_t got[300];
    int ngot;
} ep_t;

struct net {
    ep_t ep[2];
    bool wrapped;
    uint32_t now;
};

static bool is_ack(const net_t *n, const uint8_t *f) { return ((n->wrapped ? f[2] : f[0]) & 0xE0) == HP_RF_KIND_ACK; }

static int t_send(void *self, const uint8_t *frame, size_t len) {
    ep_t *e = self;
    ep_t *peer = &e->net->ep[1 - e->idx];
    bool ack = is_ack(e->net, frame);
    if (e->refuse > 0) {
        e->refuse--;
        return -HP_E_IO;
    }
    if (ack ? e->drop_acks > 0 : e->drop_objs > 0) {
        if (ack) e->drop_acks--;
        else e->drop_objs--;
        return 0;
    }
    if (e->drop_all) return 0;
    for (int copies = (e->dup_next > 0 && !ack) ? 2 : 1; copies > 0; copies--) {
        if (peer->n == QCAP) return -HP_E_BUSY;
        wire_t *w = &peer->q[(peer->head + peer->n++) % QCAP];
        memcpy(w->f, frame, len);
        w->len = len;
    }
    if (e->dup_next > 0 && !ack) e->dup_next--;
    if (ack) e->sent_acks++;
    else {
        e->sent_objs++;
        if (e->nat < 64) e->at[e->nat++] = e->net->now;
    }
    return 0;
}

static int t_txs(void *self, uint8_t *slot, uint8_t *status) {
    ep_t *e = self;
    if (!e->txs_n) return 0;
    *slot = e->txs[0][0];
    *status = e->txs[0][1];
    memmove(e->txs[0], e->txs[1], (size_t)(--e->txs_n) * 2);
    return 1;
}

static int t_recv(void *self, uint8_t *frame, size_t cap, int timeout_ms) {
    ep_t *e = self;
    (void)timeout_ms;
    if (e->n) {
        wire_t *w = &e->q[e->head];
        e->head = (e->head + 1) % QCAP;
        e->n--;
        if (w->len > cap) return -HP_E_TOOBIG;
        memcpy(frame, w->f, w->len);
        return (int)w->len;
    }
    if (e->gone) {
        e->gone = 0;
        return -HP_E_NOENT;
    }
    return 0;
}

static int t_claim(void *self) { (void)self; return 0; }

static void deliver(void *ctx, const hp_rf_msg_t *m) {
    ep_t *e = ctx;
    if (e->ngot < 300) e->got[e->ngot++] = *m;
}

static void net_init(net_t *n, bool wrapped) {
    memset(n, 0, sizeof *n);
    n->wrapped = wrapped;
    for (int i = 0; i < 2; i++) {
        ep_t *e = &n->ep[i];
        e->net = n;
        e->idx = i;
        e->t = (hp_rf_transport){e, t_send, t_recv, t_claim, NULL, NULL};
        hp_rf_link_init(&e->link, &e->t, wrapped, 1, deliver, e);
    }
    n->now = 1000;
}

/* Advances the clock in 5 ms steps, pumping both links each step. */
static void run(net_t *n, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
        n->now += 5;
        for (int i = 0; i < 2; i++) (void)hp_rf_link_pump(&n->ep[i].link, n->now);
    }
}

static int send_obj(net_t *n, int from, int type, const uint8_t *b, size_t len) {
    return hp_rf_link_send(&n->ep[from].link, (hp_msg_type_t)type, b, len, n->now);
}

static void test_basic_both_directions(bool wrapped) {
    net_t n;
    net_init(&n, wrapped);
    uint8_t a[3] = {1, 2, 3}, b[40];
    for (size_t i = 0; i < sizeof b; i++) b[i] = (uint8_t)(i * 3);
    /* both directions at once; each endpoint's seq counter is independent */
    for (int i = 0; i < 3; i++) {
        CT_EQ(send_obj(&n, 0, 10 + i, a, sizeof a), 0);
        CT_EQ(send_obj(&n, 1, 20 + i, b, sizeof b), 0);
        run(&n, 50);
    }
    run(&n, 500);
    CT_EQ(n.ep[1].ngot, 3);
    CT_EQ(n.ep[0].ngot, 3);
    for (int i = 0; i < 3; i++) {
        CT_EQ(n.ep[1].got[i].type, 10 + i);
        CT_EQ(n.ep[1].got[i].len, sizeof a);
        CT_MEM_EQ(n.ep[1].got[i].payload, a, sizeof a);
        CT_EQ(n.ep[0].got[i].type, 20 + i);
        CT_MEM_EQ(n.ep[0].got[i].payload, b, sizeof b);
        CT_EQ(n.ep[0].got[i].seq, n.ep[1].got[i].seq); /* same counter value, different counters */
    }
    CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, 0);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
    CT_EQ(hp_rf_link_status(&n.ep[1].link).failures, 0);
    CT_EQ(hp_rf_link_next_timeout_ms(&n.ep[0].link, n.now), -1);
}

static void test_lost_ack_delivered_once(void) {
    for (int w = 0; w < 2; w++) {
        net_t n;
        net_init(&n, w);
        uint8_t body[60];
        for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i + 7);
        n.ep[1].drop_acks = 1; /* B's first ack is lost: A retries the whole object */
        CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
        run(&n, 1200);
        CT_EQ(n.ep[1].ngot, 1); /* delivered exactly once */
        CT_EQ(n.ep[1].got[0].len, sizeof body);
        CT_MEM_EQ(n.ep[1].got[0].payload, body, sizeof body);
        CT_EQ(n.ep[1].sent_acks, 1); /* the retry was re-acked */
        CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
        CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, 0);
        CT_EQ(n.ep[0].sent_objs, 6); /* 3 frames, sent twice */
    }
}

static void test_retries_then_failure(void) {
    net_t n;
    net_init(&n, true);
    uint8_t body[3] = {9, 9, 9};
    n.ep[1].drop_all = 1; /* the peer never acks */
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
    run(&n, HP_RF_RETRY_MS - 10);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
    CT_EQ(n.ep[0].sent_objs, 1);
    CT_EQ(hp_rf_link_next_timeout_ms(&n.ep[0].link, n.now) >= 0, 1);
    run(&n, 10 + HP_RF_RETRY_MS * HP_RF_RETRIES + 20);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 1); /* failed after 3 retries */
    CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, 0);
    CT_EQ(n.ep[0].sent_objs, 1 + HP_RF_RETRIES);
    CT_EQ(n.ep[1].ngot, 1); /* the peer got it once; retries were duplicates */
    n.ep[1].drop_all = 0; /* the link recovers for the next object */
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
    run(&n, 100);
    CT_EQ(n.ep[1].ngot, 2);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 1);
}

static void test_duplicate_first_frame(void) {
    net_t n;
    net_init(&n, true);
    uint8_t body[70];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)i;
    n.ep[0].dup_next = 1; /* the radio delivers the first frame twice */
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
    run(&n, 300);
    CT_EQ(n.ep[1].ngot, 1);
    CT_MEM_EQ(n.ep[1].got[0].payload, body, sizeof body);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
}

static void test_every_body_size(void) {
    for (int w = 0; w < 2; w++) {
        net_t n;
        net_init(&n, w);
        for (size_t len = 0; len <= HP_RF_BODY_MAX; len++) {
            uint8_t body[HP_RF_BODY_MAX];
            for (size_t i = 0; i < len; i++) body[i] = (uint8_t)(i * 7 + len);
            CT_EQ(send_obj(&n, 0, (int)(len & 0xFF), body, len), 0);
            run(&n, 40 + HP_LINK_GAP_MS * HP_RF_MAX_FRAGS); /* frames are paced */
            CT_EQ(n.ep[1].ngot, (int)len + 1);
            if (n.ep[1].ngot != (int)len + 1) return;
            const hp_rf_msg_t *m = &n.ep[1].got[len];
            CT_EQ(m->len, len);
            CT_EQ(m->type, len & 0xFF);
            CT_MEM_EQ(m->payload, body, len);
        }
        CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
    }
}

static void test_limits_and_queue(void) {
    net_t n;
    net_init(&n, true);
    uint8_t body[HP_RF_BODY_MAX + 1] = {0};
    CT_EQ(send_obj(&n, 0, 1, body, sizeof body), -HP_E_TOOBIG);
    CT_EQ(send_obj(&n, 0, 1, NULL, 1), -HP_E_INVAL);
    CT_EQ(send_obj(&n, 0, 300, body, 1), -HP_E_INVAL);
    n.ep[1].drop_all = 1; /* nothing is acked: the queue fills */
    for (int i = 0; i < HP_LINK_QUEUE; i++) CT_EQ(send_obj(&n, 0, 1, body, 1), 0);
    CT_EQ(send_obj(&n, 0, 1, body, 1), -HP_E_BUSY);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, HP_LINK_QUEUE);
    run(&n, HP_RF_RETRY_MS * (HP_RF_RETRIES + 1) + 10); /* the head fails after its retries */
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 1);
    n.ep[1].drop_all = 0;
    run(&n, 3000); /* the rest then get through in order */
    CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, 0);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 1);
    CT_EQ(n.ep[1].ngot, HP_LINK_QUEUE); /* every object delivered exactly once, the head despite its failed acks */
}

static void test_first_frame_refused_is_retried(void) {
    net_t n;
    net_init(&n, false);
    uint8_t body[2] = {4, 5};
    n.ep[0].refuse = 2; /* transport busy (e.g. write backoff) for the first two attempts */
    CT_EQ(send_obj(&n, 0, 3, body, sizeof body), 0);
    run(&n, 1000);
    CT_EQ(n.ep[1].ngot, 1);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
    n.ep[0].refuse = 100; /* never works: dropped and counted after the attempts run out */
    CT_EQ(send_obj(&n, 0, 3, body, sizeof body), 0);
    run(&n, 2000);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 1);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).queued, 0);
}

static void test_peer_lost_resets(void) {
    net_t n;
    net_init(&n, true);
    uint8_t big[60] = {1}, body[2] = {1, 2};
    CT_EQ(send_obj(&n, 0, 5, big, sizeof big), 0);
    run(&n, 100);
    CT_EQ(n.ep[1].ngot, 1);
    n.ep[0].drop_all = 1;
    CT_EQ(send_obj(&n, 1, 6, body, sizeof body), 0); /* B's object is in flight, unacked */
    n.ep[1].drop_all = 1;
    n.ep[1].gone = 1;
    CT_EQ(hp_rf_link_pump(&n.ep[1].link, n.now), -HP_E_NOENT);
    CT_EQ(hp_rf_link_status(&n.ep[1].link).failures, 1);
    CT_EQ(hp_rf_link_status(&n.ep[1].link).queued, 0);
    CT_EQ(hp_rf_link_pump(&n.ep[1].link, n.now), 0); /* once */
    /* after a reset the same seq is accepted again (the duplicate filter was cleared) */
    n.ep[0].drop_all = n.ep[1].drop_all = 0;
    CT_EQ(send_obj(&n, 0, 5, big, sizeof big), 0);
    run(&n, 200);
    CT_EQ(n.ep[1].ngot, 2);
}

static int noent_send(void *self, const uint8_t *f, size_t l) { (void)self; (void)f; (void)l; return -HP_E_NOENT; }
static int idle_recv(void *self, uint8_t *f, size_t c, int t) { (void)self; (void)f; (void)c; (void)t; return 0; }

static void test_noent_synchronous(void) {
    hp_rf_transport t = {NULL, noent_send, idle_recv, t_claim, NULL, NULL};
    hp_rf_link_t l;
    uint8_t b[1] = {0};
    hp_rf_link_init(&l, &t, true, 1, NULL, NULL);
    CT_EQ(hp_rf_link_send(&l, HP_MSG_KEY_EVENT, b, 1, 0), -HP_E_NOENT); /* nobody there: reported at once */
    CT_EQ(hp_rf_link_status(&l).queued, 0);
    CT_EQ(hp_rf_link_status(&l).failures, 0);
}

/* ---- pacing ---- */

static void add_status(ep_t *e, uint8_t slot, uint8_t st) {
    e->txs[e->txs_n][0] = slot;
    e->txs[e->txs_n][1] = st;
    e->txs_n++;
}

static void test_pacing_gap(void) {
    for (int frames = 2; frames <= 3; frames++) {
        net_t n;
        net_init(&n, false);
        uint8_t body[60 + 25];
        memset(body, 7, sizeof body);
        size_t len = frames == 2 ? 30 : 60; /* 1+30 = 2 chunks, 1+60 = 3 chunks */
        CT_EQ(send_obj(&n, 0, 5, body, len), 0);
        CT_EQ(n.ep[0].sent_objs, 1); /* frame 1 at once */
        run(&n, HP_LINK_GAP_MS - 5);
        CT_EQ(n.ep[0].sent_objs, 1); /* nothing before the gap */
        run(&n, 5);
        CT_EQ(n.ep[0].sent_objs, 2);
        if (frames == 3) {
            run(&n, HP_LINK_GAP_MS);
            CT_EQ(n.ep[0].sent_objs, 3);
            CT_EQ(n.ep[0].at[2] - n.ep[0].at[1], HP_LINK_GAP_MS);
        }
        CT_EQ(n.ep[0].at[1] - n.ep[0].at[0], HP_LINK_GAP_MS);
        run(&n, 100);
        CT_EQ(n.ep[1].ngot, 1);
        CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
        CT_EQ(n.ep[0].sent_objs, frames); /* no retry: the ack arrived */
        CT_EQ(hp_rf_link_next_timeout_ms(&n.ep[0].link, n.now), -1);
    }
}

static void test_pacing_tx_status(void) {
    net_t n;
    net_init(&n, false);
    n.ep[0].t.tx_status = t_txs;
    n.ep[0].link.tx_slot = 2;
    uint8_t body[60] = {0};
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0); /* 3 frames */
    CT_EQ(n.ep[0].sent_objs, 1);
    add_status(&n.ep[0], 3, 0x00); /* another slot: does not release */
    run(&n, 10);
    CT_EQ(n.ep[0].sent_objs, 1);
    add_status(&n.ep[0], 2, 0x01); /* ours: frame 2 well before the gap */
    run(&n, 5);
    CT_EQ(n.ep[0].sent_objs, 2);
    CT_CHECK(n.ep[0].at[1] - n.ep[0].at[0] < HP_LINK_GAP_MS);
    run(&n, 5); /* a status must be for the NEW frame: none arrived, so the gap rules */
    CT_EQ(n.ep[0].sent_objs, 2);
    add_status(&n.ep[0], 2, 0x00);
    run(&n, 5);
    CT_EQ(n.ep[0].sent_objs, 3);
    run(&n, 100);
    CT_EQ(n.ep[1].ngot, 1);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
}

static void test_pacing_single_frame_unaffected(void) {
    net_t n;
    net_init(&n, false);
    uint8_t b[3] = {1, 2, 3};
    CT_EQ(send_obj(&n, 0, 5, b, sizeof b), 0);
    CT_EQ(n.ep[0].sent_objs, 1);
    run(&n, 5);
    CT_EQ(n.ep[1].ngot, 1);
    CT_EQ(n.ep[0].sent_objs, 1);
    n.ep[1].drop_acks = 1; /* retry still resends the single frame after the retry time */
    CT_EQ(send_obj(&n, 0, 5, b, sizeof b), 0);
    run(&n, HP_RF_RETRY_MS + 20);
    CT_EQ(n.ep[0].sent_objs, 3);
    CT_EQ(n.ep[1].ngot, 2);
}

static void test_pacing_retry_repaces(void) {
    net_t n;
    net_init(&n, false);
    uint8_t body[60] = {0};
    n.ep[1].drop_acks = 1;
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
    run(&n, 1000);
    CT_EQ(n.ep[0].sent_objs, 6); /* 3 + 3 */
    CT_EQ(n.ep[1].ngot, 1);
    CT_EQ(hp_rf_link_status(&n.ep[0].link).failures, 0);
    CT_CHECK(n.ep[0].at[4] - n.ep[0].at[3] >= HP_LINK_GAP_MS); /* the retry is paced too */
}

static void cap_log(void *ctx, const char *line) {
    char *buf = ctx;
    strncat(buf, line, 2000 - strlen(buf) - 2);
    strcat(buf, "\n");
}

static void test_link_debug_log(void) {
    net_t n;
    net_init(&n, false);
    static char buf[2048];
    buf[0] = 0;
    hp_rf_link_set_log(&n.ep[0].link, cap_log, buf);
    uint8_t body[30] = {0};
    CT_EQ(send_obj(&n, 0, 5, body, sizeof body), 0);
    run(&n, 100);
    CT_CHECK(strstr(buf, "send start type 5"));
    CT_CHECK(strstr(buf, "frame 1/2 handed"));
    CT_CHECK(strstr(buf, "frame 2/2 handed"));
    CT_CHECK(strstr(buf, "rx ack type 5"));
    CT_CHECK(strstr(buf, "tx done"));
}

int main(void) {
    test_basic_both_directions(true);
    test_basic_both_directions(false);
    test_lost_ack_delivered_once();
    test_retries_then_failure();
    test_duplicate_first_frame();
    test_every_body_size();
    test_limits_and_queue();
    test_first_frame_refused_is_retried();
    test_peer_lost_resets();
    test_noent_synchronous();
    test_pacing_gap();
    test_pacing_tx_status();
    test_pacing_single_frame_unaffected();
    test_pacing_retry_repaces();
    test_link_debug_log();
    return CT_DONE();
}
