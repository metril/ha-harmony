/* hub_pump_rf: `remote_attached` / `remote_info` ordering against a transport whose reader thread (modelled by
 * the test) can attach, lose and re-attach at any point of a pump. Link loss is an in-order marker in the receive
 * queue (like rf_spi). Invariant under test: per link-up exactly one `remote_attached`, any remote_info of that
 * link after it, nothing of a new link wiped by the loss of the old one. */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/rf/link.h"
#include "hub/rf/rf_payload.h"

#define QN 64
typedef struct {
    bool lost;
    uint8_t f[HP_RF_INNER_LEN];
} ent_t;

static ent_t Q[QN];
static int qh, qn;
static bool phys; /* the transport's attached flag */
static int recv_calls, att_calls;
static void (*on_recv)(int call);
static void (*on_att)(int call); /* runs inside attached(), after it computed its answer */

static hub_ctx_t H;
static hp_rf_link_t RL; /* the remote end: only used to make wire-accurate frames */
static uint32_t now_ms = 1000;
static ent_t cap[QN];
static int ncap;

static char LOG[64][24];
static int nlog;
static void rec(void *ctx, const char *type, const cJSON *d) {
    (void)ctx;
    (void)d;
    if (nlog < 64) snprintf(LOG[nlog++], sizeof LOG[0], "%s", type);
}

static void qpush(const ent_t *e) {
    if (qn < QN) Q[(qh + qn++) % QN] = *e;
}
/* reader thread events */
static void r_up(void) { phys = true; }
static void r_lost(void) {
    if (!phys) return;
    phys = false;
    ent_t e = {.lost = true};
    qpush(&e);
}
static void r_frames(int from, int n) { /* data frames: they attach the link */
    phys = true;
    for (int i = from; i < from + n; i++) qpush(&cap[i]);
}

static int t_recv(void *self, uint8_t *f, size_t capn, int ms) {
    (void)self; (void)capn; (void)ms;
    recv_calls++;
    if (on_recv) on_recv(recv_calls);
    if (!qn) return 0;
    ent_t e = Q[qh];
    qh = (qh + 1) % QN;
    qn--;
    if (e.lost) return -HP_E_NOENT;
    memcpy(f, e.f, HP_RF_INNER_LEN);
    return HP_RF_INNER_LEN;
}
static int t_send(void *self, const uint8_t *f, size_t n) { (void)self; (void)f; (void)n; return 0; }
static int t_claim(void *self) { (void)self; return 0; }
static bool t_att(void *self) {
    (void)self;
    bool a = phys;
    att_calls++;
    if (on_att) on_att(att_calls);
    return a;
}
static hp_rf_transport HT;

/* the remote end captures what it transmits */
static int rt_send(void *self, const uint8_t *f, size_t n) {
    (void)self;
    CT_EQ(n, HP_RF_INNER_LEN);
    if (ncap < QN) {
        cap[ncap].lost = false;
        memcpy(cap[ncap++].f, f, HP_RF_INNER_LEN);
    }
    return 0;
}
static int rt_recv(void *self, uint8_t *f, size_t c, int ms) { (void)self; (void)f; (void)c; (void)ms; return 0; }
static hp_rf_transport RT;
static void rem_deliver(void *ctx, const hp_rf_msg_t *m) { (void)ctx; (void)m; }

static uint32_t clk(void *c) { (void)c; return now_ms; }

/* Builds the frames of one remote_info / battery object; returns the index of the first. */
static int make_obj(hp_msg_type_t type, const hp_rf_val_t *v, size_t nv) {
    int first = ncap;
    hp_rf_link_init(&RL, &RT, false, 0, rem_deliver, NULL); /* one object in flight per link: a fresh end each */
    static uint8_t seqn;
    (void)hp_rf_link_pump(&RL, now_ms); /* lazy tx init first */
    RL.tx.next_seq = ++seqn; /* the hub's link drops a repeated sequence number as a duplicate */
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    CT_EQ(hp_rf_pack(type, v, nv, pl, sizeof pl, &n), 0);
    CT_EQ(hp_rf_link_send(&RL, type, pl, n, now_ms), 0);
    now_ms += 50;
    (void)hp_rf_link_pump(&RL, now_ms);
    CT_CHECK(ncap > first);
    return first;
}
static int make_info(uint32_t caps) {
    hp_rf_val_t v[6] = {{.u = 1}, {.u = 2}, {.u = caps}, {.u = 50}, {.u = 0}, {.u = 0}};
    return make_obj(HP_MSG_REMOTE_INFO, v, 6);
}
static int make_battery(void) {
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 80}, {.u = 3000}};
    return make_obj(HP_MSG_BATTERY, v, 3);
}
static int nframes_since(int first) { return ncap - first; }

static void world(void) {
    memset(Q, 0, sizeof Q);
    qh = qn = 0;
    phys = false;
    recv_calls = att_calls = 0;
    on_recv = NULL;
    on_att = NULL;
    nlog = 0;
    ncap = 0;
    hub_ctx_init(&H);
    hub_set_clock(&H, clk, NULL);
    HT = (hp_rf_transport){NULL, t_send, t_recv, t_claim, t_att, NULL, NULL};
    RT = (hp_rf_transport){NULL, rt_send, rt_recv, t_claim, NULL, NULL, NULL};
    H.sinks.rf = &HT;
    H.rf_inner_blocks = true;
    H.bcast_hook = rec;
    hp_rf_link_init(&RL, &RT, false, 0, rem_deliver, NULL);
}

static void pump(void) {
    now_ms += 5;
    hub_pump_rf(&H);
}
static int count(const char *t) {
    int c = 0;
    for (int i = 0; i < nlog; i++)
        if (!strcmp(LOG[i], t)) c++;
    return c;
}
/* the broadcast sequence as one string "a,i,..." (a=remote_attached, i=remote_info, b=battery) */
static const char *seq(void) {
    static char s[128];
    s[0] = 0;
    for (int i = 0; i < nlog; i++) {
        const char *c = !strcmp(LOG[i], "remote_attached") ? "a" : !strcmp(LOG[i], "remote_info") ? "i" : !strcmp(LOG[i], "battery") ? "b" : "?";
        strcat(s, c);
    }
    return s;
}
static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* 1. the reader attaches and queues remote_info between the pump's checks: attached still comes first */
static int info_first;
static void attach_in_recv(int call) {
    if (call == 1) r_frames(info_first, nframes_since(info_first) > 0 ? 1 : 0);
}
static void test_attach_during_pump_recv(void) {
    world();
    info_first = make_info(3);
    int n = nframes_since(info_first);
    CT_CHECK(n >= 1);
    pump(); /* nothing yet */
    CT_EQ(nlog, 0);
    r_frames(info_first, n);
    pump();
    CT_CHECK(streq(seq(), "ai"));
    CT_EQ(H.info_bcast, 1);
    CT_CHECK(hub_remote_info(&H, 1) != NULL);
}

/* 1b. the reader attaches inside the pump, after the pump read the empty queue and before the flag is checked */
static void attach_after_recv(int call) {
    (void)call;
    static bool done;
    if (!done && recv_calls >= 1) {
        done = true;
        r_frames(info_first, nframes_since(info_first));
    }
}
static void test_attach_between_recv_and_flag(void) {
    world();
    info_first = make_info(3);
    on_att = attach_after_recv; /* runs in attached(): the flag answer was computed before the frames arrive */
    pump();
    /* this pump saw the flag false (stale): no event; the frames are for the next pump */
    CT_CHECK(streq(seq(), ""));
    on_att = NULL;
    pump();
    CT_CHECK(streq(seq(), "ai")); /* announced once, before the info the frames carried */
    pump();
    pump();
    CT_CHECK(streq(seq(), "ai"));
}

/* 1c. the reader attaches inside the first recv of the pump: the frame is delivered by that pump */
static void test_attach_inside_recv(void) {
    world();
    info_first = make_info(3);
    on_recv = attach_in_recv;
    pump();
    CT_CHECK(streq(seq(), "ai")); /* attached first, although the frame is what told the hub */
    CT_EQ(H.info_bcast, 1);
}

/* 2. old link's frames, its loss, the new link's frames, all queued before the pump looks */
static void test_loss_and_new_session_frames_are_ordered(void) {
    world();
    int i1 = make_info(1);
    int n1 = nframes_since(i1);
    r_frames(i1, n1);
    pump();
    CT_CHECK(streq(seq(), "ai"));
    int b1 = make_battery(); /* last data of the old link */
    int bn = nframes_since(b1);
    int i2 = make_info(7); /* what the NEW link's remote says first */
    int n2 = nframes_since(i2);
    r_frames(b1, bn);
    r_lost();
    r_up();
    r_frames(i2, n2);
    pump(); /* the old battery, then the loss, then (tail) the new link-up */
    CT_CHECK(streq(seq(), "aiba")); /* old link's last frames, its loss in order, then the new link's attach */
    pump();
    CT_CHECK(streq(seq(), "aibai"));
    CT_CHECK(hub_remote_info(&H, 1) != NULL && hub_remote_info(&H, 1)->caps == 7); /* not wiped by the old loss */
    CT_EQ(count("remote_attached"), 2);
}

/* 3. no frames: lost and up (twice) between pumps = one announcement per consumed link-up, never two per pump */
static void test_no_double_attach(void) {
    world();
    r_up();
    pump();
    pump();
    CT_EQ(count("remote_attached"), 1);
    r_lost();
    r_up();
    pump();
    pump();
    CT_EQ(count("remote_attached"), 2);
    r_lost(); /* a loss nobody sees a re-attach of */
    pump();
    pump();
    CT_EQ(count("remote_attached"), 2);
    CT_CHECK(!H.rf_attached);
    r_up();
    pump();
    CT_EQ(count("remote_attached"), 3);
}

/* 4. a link-up with remote_info already cached (a path that keeps the cache): re-broadcast after remote_attached */
static void test_cached_info_follows_the_announcement(void) {
    world();
    int i1 = make_info(3);
    r_frames(i1, nframes_since(i1));
    pump();
    CT_CHECK(streq(seq(), "ai"));
    H.rf_attached = false; /* the hub is told of a link-up it had not seen go down */
    nlog = 0;
    pump();
    CT_CHECK(streq(seq(), "ai")); /* attached, then the cached info again */
    CT_EQ(H.info_bcast, 2);
    pump();
    CT_CHECK(streq(seq(), "ai"));
}

/* 5. frames queued before a loss are delivered, then the loss clears the old link's info */
static void test_frames_before_loss_delivered_then_cleared(void) {
    world();
    int i1 = make_info(3);
    r_frames(i1, nframes_since(i1));
    int b1 = make_battery();
    r_frames(b1, nframes_since(b1));
    r_lost();
    pump();
    CT_CHECK(streq(seq(), "aib"));
    CT_CHECK(hub_remote_info(&H, 1) == NULL);
    CT_CHECK(!H.rf_attached);
    pump();
    CT_CHECK(streq(seq(), "aib"));
}

/* 6. a frame of a link with no earlier signal: announced before it, once */
static void test_first_frame_announces_first(void) {
    world();
    int b1 = make_battery();
    r_frames(b1, nframes_since(b1));
    pump();
    CT_CHECK(streq(seq(), "ab"));
    int b2 = make_battery();
    r_frames(b2, nframes_since(b2));
    pump();
    CT_CHECK(streq(seq(), "abb"));
}

int main(void) {
    test_attach_during_pump_recv();
    test_attach_between_recv_and_flag();
    test_attach_inside_recv();
    test_loss_and_new_session_frames_are_ordered();
    test_no_double_attach();
    test_cached_info_follows_the_announcement();
    test_frames_before_loss_delivered_then_cleared();
    test_first_frame_announces_first();
    return CT_DONE();
}
