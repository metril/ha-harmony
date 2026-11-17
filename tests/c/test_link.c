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
    else e->sent_objs++;
    return 0;
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
        e->t = (hp_rf_transport){e, t_send, t_recv, t_claim, NULL};
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
        run(&n, 1000);
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
            run(&n, 40);
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
    hp_rf_transport t = {NULL, noent_send, idle_recv, t_claim, NULL};
    hp_rf_link_t l;
    uint8_t b[1] = {0};
    hp_rf_link_init(&l, &t, true, 1, NULL, NULL);
    CT_EQ(hp_rf_link_send(&l, HP_MSG_KEY_EVENT, b, 1, 0), -HP_E_NOENT); /* nobody there: reported at once */
    CT_EQ(hp_rf_link_status(&l).queued, 0);
    CT_EQ(hp_rf_link_status(&l).failures, 0);
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
    return CT_DONE();
}
