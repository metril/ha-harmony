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
static int att_remote = -2; /* the `remote` of the last remote_attached (-1: the field was absent) */
static void rec(void *ctx, const char *type, const cJSON *d) {
    (void)ctx;
    if (!strcmp(type, "remote_attached")) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "remote");
        att_remote = cJSON_IsNumber(r) ? (int)r->valuedouble : -1;
    }
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
static uint32_t g_rx_at; /* the stamp frame_times reports for the frame recv() returned last */
static int t_times(void *self, uint32_t *rx_at, uint32_t *tx_at) {
    (void)self;
    *rx_at = g_rx_at;
    *tx_at = 0;
    return 1;
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
static uint32_t g_boot; /* the nonce the fake remote reports; a change is a reboot */
static int make_info_gen(uint32_t caps, uint32_t gen) {
    hp_rf_val_t v[7] = {{.u = 1}, {.u = 2}, {.u = caps}, {.u = 50}, {.u = gen}, {.u = 2}, {.u = g_boot}};
    return make_obj(HP_MSG_REMOTE_INFO, v, 7);
}
static int make_info(uint32_t caps) { return make_info_gen(caps, 0); }
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
    int i2 = make_info_gen(7, 1); /* what the NEW link's remote says first (another page set: not a flap) */
    int n2 = nframes_since(i2);
    r_frames(b1, bn);
    r_lost();
    r_up();
    r_frames(i2, n2);
    pump(); /* the old battery, then the loss, then (tail) the new link-up */
    CT_CHECK(streq(seq(), "aib")); /* old link's last frames, its loss in order; the flap re-up waits for the info */
    pump();
    CT_CHECK(streq(seq(), "aibai"));
    CT_CHECK(hub_remote_info(&H, 1) != NULL && hub_remote_info(&H, 1)->caps == 7); /* not wiped by the old loss */
    CT_EQ(count("remote_attached"), 2);
}

/* 2b. a flap (re-up within 10 s, same remote_info gen): no remote_attached and no cover resend; a changed gen, or a
 * re-up after the window, is announced as before. The announcement names the remote. */
static void test_flap_is_not_announced(void) {
    world();
    int i1 = make_info_gen(3, 5);
    r_frames(i1, nframes_since(i1));
    pump();
    CT_CHECK(streq(seq(), "ai"));
    CT_EQ(att_remote, 1); /* named by the remote_info that revealed the link */
    H.cover[0].used = true;
    H.cover[0].nsent = 5;
    H.cover[0].hold = false;
    r_lost();
    r_up();
    pump();
    CT_CHECK(streq(seq(), "ai")); /* re-up seen, nothing yet */
    int i2 = make_info_gen(3, 5);
    r_frames(i2, nframes_since(i2));
    pump();
    CT_CHECK(streq(seq(), "aii")); /* the info follows, the announcement does not */
    CT_EQ(count("remote_attached"), 1);
    CT_EQ(H.cover[0].nsent, 5); /* the cover is not re-armed */
    CT_CHECK(!H.cover[0].hold);
    /* a flap that returns with another gen is a new remote state: announced, covers re-armed */
    r_lost();
    r_up();
    int i3 = make_info_gen(3, 9);
    r_frames(i3, nframes_since(i3));
    pump();
    pump();
    CT_EQ(count("remote_attached"), 2);
    CT_CHECK(H.cover[0].nsent < 5); /* started over (the flush may already have sent a chunk) */
    /* same gen but after the window: announced */
    pump();
    r_lost();
    pump();
    now_ms += 11000;
    r_up();
    int i4 = make_info_gen(3, 9);
    r_frames(i4, nframes_since(i4));
    pump();
    pump();
    CT_EQ(count("remote_attached"), 3);
    /* a remote that REBOOTED inside the window (new nonce, same gen and pages) lost its covers: served */
    r_lost();
    pump();
    r_up();
    g_boot = 0x1234;
    int ib = make_info_gen(3, 9);
    r_frames(ib, nframes_since(ib));
    pump();
    pump();
    CT_EQ(count("remote_attached"), 4);
    r_lost();
    pump();
    r_up(); /* and a flap of that same boot is again not announced */
    int ic = make_info_gen(3, 9);
    r_frames(ic, nframes_since(ic));
    pump();
    pump();
    CT_EQ(count("remote_attached"), 4);
    /* a remote that comes back holding no pages (it lost its store) is always served, whatever the gen */
    r_lost();
    hp_rf_val_t e[7] = {{.u = 1}, {.u = 2}, {.u = 3}, {.u = 50}, {.u = 9}, {.u = 0}, {.u = g_boot}};
    int i5 = make_obj(HP_MSG_REMOTE_INFO, e, 7);
    r_up();
    r_frames(i5, nframes_since(i5));
    pump();
    pump();
    CT_EQ(count("remote_attached"), 5);
}

/* 2c. a link-up the transport's flag revealed (no frame) names the last remote seen, none before any */
static void test_flag_attach_names_the_last_remote(void) {
    world();
    att_remote = -2;
    r_up();
    pump();
    CT_EQ(att_remote, -1); /* no remote seen yet: the field is absent */
    int b = make_battery();
    r_frames(b, nframes_since(b));
    pump();
    r_lost();
    pump();
    now_ms += 11000;
    r_up();
    pump();
    CT_EQ(count("remote_attached"), 2);
    CT_EQ(att_remote, 1);
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

/* a new link starts with nothing in flight, whatever a previous link left in the counters */
static void test_link_up_resets_in_flight_counters(void) {
    world();
    H.theme_out = 2;
    H.page_out[3] = 1;
    int b = make_battery();
    r_frames(b, nframes_since(b));
    pump();
    CT_EQ(H.theme_out, 0);
    CT_EQ(H.page_out[3], 0);
}

/* a silent remote accepted by a transport probe after the pump looked (rf_up in a flush or timer) is announced by
 * the very next loop iteration: the poll must not sleep on it, nothing else would wake the hub */
static void test_silent_attach_after_pump_wakes_the_loop(void) {
    world();
    pump();
    CT_CHECK(streq(seq(), ""));
    r_up(); /* accepted outside the pump's own check; the remote sends nothing */
    CT_EQ(hub_next_timeout_ms(&H), 0);
    pump();
    CT_CHECK(streq(seq(), "a"));
    CT_CHECK(hub_next_timeout_ms(&H) != 0);
}

/* A key frame stamped `age` ms before the pump: how many key_event broadcasts the pump produced. */
static int key_with_age(unsigned action, uint32_t age) {
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 5}, {.u = action}};
    int first = make_obj(HP_MSG_KEY_EVENT, v, 3);
    int before = count("key_event");
    r_frames(first, nframes_since(first));
    g_rx_at = now_ms + 5 - age; /* pump() advances the clock by 5 */
    pump();
    return count("key_event") - before;
}

/* Stale presses and holds are dropped (not executed, not emitted); a release is always processed. */
static void test_stale_keys_dropped_release_kept(void) {
    world();
    HT.frame_times = t_times;
    CT_EQ(key_with_age(HP_KEY_ACTION_PRESS, 100), 1);
    CT_EQ(key_with_age(HP_KEY_ACTION_PRESS, 1500), 1);  /* at the limit: kept */
    CT_EQ(key_with_age(HP_KEY_ACTION_PRESS, 1600), 0);  /* stale press */
    CT_EQ(key_with_age(HP_KEY_ACTION_HOLD, 5000), 0);   /* stale hold */
    CT_EQ(key_with_age(HP_KEY_ACTION_RELEASE, 9000), 1); /* a release is never dropped */
    HT.frame_times = NULL;
    CT_EQ(key_with_age(HP_KEY_ACTION_PRESS, 9000), 1);  /* no stamps: nothing can be judged */
}

int main(void) {
    test_stale_keys_dropped_release_kept();
    test_attach_during_pump_recv();
    test_attach_between_recv_and_flag();
    test_attach_inside_recv();
    test_loss_and_new_session_frames_are_ordered();
    test_flap_is_not_announced();
    test_flag_attach_names_the_last_remote();
    test_no_double_attach();
    test_cached_info_follows_the_announcement();
    test_frames_before_loss_delivered_then_cleared();
    test_first_frame_announces_first();
    test_link_up_resets_in_flight_counters();
    test_silent_attach_after_pump_wakes_the_loop();
    return CT_DONE();
}
