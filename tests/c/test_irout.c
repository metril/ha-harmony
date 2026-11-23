/* IR output runtime: key hold/repeat, 2 s cap, SIRC x3, RC5/RC6 toggle, driven by a fake clock. */
#include "ctest_assert.h"
#include "hub/engine/irout.h"
#include "hub/engine/mapping.h"
#include "hub/hp_errors.h"

static uint32_t g_now;
static uint32_t clk(void *c) { (void)c; return g_now; }

typedef struct {
    uint32_t t;
    uint8_t port;
    uint16_t n;
    uint32_t hz;
    uint16_t us0, us1;
    uint32_t h; /* FNV-1a over the once-part */
} rec_t;

static rec_t recs[256];
static size_t nrec;

static uint32_t hash_us(const uint16_t *us, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ us[i]) * 16777619u;
    return h;
}

static int rec_send(void *self, uint8_t port, const hp_ir_signal_t *sig) {
    (void)self;
    CT_CHECK(nrec < 256);
    rec_t *r = &recs[nrec++ % 256];
    r->t = g_now;
    r->port = port;
    r->n = sig->once_n;
    r->hz = sig->carrier_hz;
    r->us0 = sig->us[0];
    r->us1 = sig->us[1];
    r->h = hash_us(sig->us, sig->once_n);
    return 0;
}

static hp_ir_sink ir = {NULL, rec_send};
static hp_sinks_t sinks = {&ir, NULL, NULL};
static hp_sched_t sched;
static hp_irout_t out;
static hp_map_t map;

/* advance the clock to `to`, running timers in 1 ms steps like a poll loop would */
static void advance(uint32_t to) {
    while (g_now < to) {
        g_now++;
        hp_sched_run(&sched, g_now);
    }
}

static void setup(const char *cfg) {
    hp_map_free(&map);
    g_now = 10000;
    nrec = 0;
    hp_sched_init(&sched, clk, NULL);
    hp_irout_init(&out, &sched);
    hp_map_init(&map);
    hp_map_set_out(&map, &out);
    int r = hp_map_load(&map, cfg, strlen(cfg));
    if (r < 0) fprintf(stderr, "load: %s\n", hp_map_last_error(&map));
    CT_EQ(r, 0);
}

static void key(uint8_t k, hp_key_action_t a) { CT_EQ(hp_key_handle(&map, &sinks, 1, k, a, NULL, NULL), 0); }

#define CFG(...) "{\"format\":2,\"keys\":{" __VA_ARGS__ "}}"

static void test_nec_hold_repeat_and_cap(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8}"));
    key(1, HP_KEY_ACTION_PRESS);
    CT_EQ(nrec, 1);
    CT_EQ(recs[0].n, 68);
    CT_EQ(recs[0].hz, 38000);
    advance(g_now + 400); /* nothing repeats before HOLD */
    CT_EQ(nrec, 1);
    uint32_t t0 = g_now;
    key(1, HP_KEY_ACTION_HOLD);
    advance(t0 + 107);
    CT_EQ(nrec, 1);
    advance(t0 + 108);
    CT_EQ(nrec, 2);
    CT_EQ(recs[1].n, 4); /* NEC repeat code, not the full frame */
    CT_EQ(recs[1].t, t0 + 108);
    advance(t0 + 216);
    CT_EQ(nrec, 3);
    CT_EQ(recs[2].t, t0 + 216);
    key(1, HP_KEY_ACTION_HOLD); /* a second HOLD does not double the rate */
    advance(t0 + 324);
    CT_EQ(nrec, 4);
    /* lost RELEASE: repeats stop at the 2 s cap */
    advance(t0 + 5000);
    size_t n = nrec;
    CT_EQ(n, 1 + 20); /* repeats at 108 .. 2160: the cap runs from the latest HOLD (t0 + 216) */
    CT_CHECK(recs[n - 1].t <= t0 + 216 + HP_IROUT_HOLD_CAP_MS);
    CT_EQ(hp_sched_pending(&sched), 0);
    advance(t0 + 8000);
    CT_EQ(nrec, n);
}

static void test_periodic_hold_extends_cap(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    /* a genuine long hold: HOLD every HP_IROUT_HOLD_INTERVAL_MS keeps repeating well past the cap */
    for (uint32_t t = 0; t < 6000; t += HP_IROUT_HOLD_INTERVAL_MS) {
        advance(t0 + t);
        key(1, HP_KEY_ACTION_HOLD);
    }
    size_t n = nrec;
    CT_CHECK(n > 40); /* ~108 ms period over 6 s */
    advance(g_now + 300);
    CT_CHECK(nrec > n); /* still repeating */
    /* the remote dies / RELEASE is lost: the repeats stop within the cap of the last HOLD */
    uint32_t last_hold = g_now - 300;
    advance(last_hold + HP_IROUT_HOLD_CAP_MS + 200);
    n = nrec;
    CT_CHECK(recs[n - 1].t <= last_hold + HP_IROUT_HOLD_CAP_MS);
    CT_CHECK(recs[n - 1].t > last_hold + HP_IROUT_HOLD_CAP_MS - 150);
    CT_EQ(hp_sched_pending(&sched), 0);
    advance(g_now + 3000);
    CT_EQ(nrec, n);
    /* HOLD without a candidate frame (after the stop) is still ignored */
    key(1, HP_KEY_ACTION_HOLD);
    advance(g_now + 500);
    CT_EQ(nrec, n);
}

static void test_release_stops_repeat(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    key(1, HP_KEY_ACTION_HOLD);
    advance(t0 + 230);
    CT_EQ(nrec, 3);
    key(1, HP_KEY_ACTION_RELEASE);
    advance(t0 + 2500);
    CT_EQ(nrec, 3);
    CT_EQ(hp_sched_pending(&sched), 0);
    key(1, HP_KEY_ACTION_HOLD); /* HOLD after RELEASE (no press) repeats nothing */
    advance(g_now + 500);
    CT_EQ(nrec, 3);
}

static void test_new_press_ends_hold(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8},"
              "\"2\":{\"type\":\"ha\",\"event\":\"x\"}"));
    key(1, HP_KEY_ACTION_PRESS);
    key(1, HP_KEY_ACTION_HOLD);
    advance(g_now + 120);
    CT_EQ(nrec, 2);
    key(2, HP_KEY_ACTION_PRESS); /* any other press ends the repeat */
    advance(g_now + 1000);
    CT_EQ(nrec, 2);
    CT_EQ(hp_sched_pending(&sched), 0);
}

static void test_sirc_three_frames(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    CT_EQ(nrec, 1);
    CT_EQ(recs[0].n, 26);
    CT_EQ(recs[0].hz, 40000);
    advance(t0 + 200);
    CT_EQ(nrec, 3); /* a single press = 3 frames */
    CT_EQ(recs[1].t, t0 + 45);
    CT_EQ(recs[2].t, t0 + 90);
    CT_EQ(recs[1].us0, recs[0].us0);
    CT_EQ(hp_sched_pending(&sched), 0);
    /* hold: the full frame repeats every 45 ms after the three */
    key(1, HP_KEY_ACTION_PRESS);
    key(1, HP_KEY_ACTION_HOLD);
    uint32_t t1 = g_now;
    advance(t1 + 150);
    CT_CHECK(nrec >= 6 + 1); /* 3 + 3 + at least one repeat */
    CT_EQ(recs[nrec - 1].n, 26);
    key(1, HP_KEY_ACTION_RELEASE);
    size_t n = nrec;
    advance(t1 + 1000);
    CT_EQ(nrec, n);
}

static void test_samsung_repeats_full_frame(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"samsung32\",\"addr\":7,\"cmd\":2}"));
    key(1, HP_KEY_ACTION_PRESS);
    key(1, HP_KEY_ACTION_HOLD);
    advance(g_now + 110);
    CT_EQ(nrec, 2);
    CT_EQ(recs[1].n, 68);
}

static void test_rc5_toggle(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"rc5\",\"addr\":5,\"cmd\":12},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"rc5\",\"addr\":6,\"cmd\":12},"
              "\"3\":{\"type\":\"ir\",\"proto\":\"rc6\",\"addr\":5,\"cmd\":12}"));
    hp_ir_signal_t s0, s1;
    hp_ir_code_t c = {.kind = HP_IR_CODE_PROTO, .u.p = {HP_IR_RC5, 5, 12}};
    CT_EQ(hp_ir_encode(&c, 0, &s0), 0);
    CT_EQ(hp_ir_encode(&c, 1, &s1), 0);
    CT_CHECK(memcmp(s0.us, s1.us, s0.once_n * 2u) != 0); /* toggle changes the waveform */

    uint32_t h0 = hash_us(s0.us, s0.once_n), h1 = hash_us(s1.us, s1.once_n);
    CT_CHECK(h0 != h1);
    key(1, HP_KEY_ACTION_PRESS);
    CT_EQ(recs[0].h, h0);
    key(1, HP_KEY_ACTION_RELEASE);
    advance(g_now + 300); /* let the line go quiet: a press during a frame is deferred */
    key(1, HP_KEY_ACTION_PRESS); /* new press flips */
    CT_EQ(recs[1].h, h1);
    key(1, HP_KEY_ACTION_HOLD); /* constant during the hold */
    advance(g_now + 350);
    CT_CHECK(nrec >= 4);
    for (size_t i = 2; i < nrec; i++) CT_EQ(recs[i].h, h1);
    key(1, HP_KEY_ACTION_RELEASE);
    advance(g_now + 300); /* let the line go quiet: a press during a frame is deferred */
    key(1, HP_KEY_ACTION_PRESS); /* and flips back */
    CT_EQ(recs[nrec - 1].h, h0);
    key(1, HP_KEY_ACTION_RELEASE);
    advance(g_now + 300); /* let the line go quiet: a press during a frame is deferred */
    /* a different address has its own toggle: first press is 0 again */
    size_t n = nrec;
    key(2, HP_KEY_ACTION_PRESS);
    hp_ir_code_t c6 = {.kind = HP_IR_CODE_PROTO, .u.p = {HP_IR_RC5, 6, 12}};
    hp_ir_signal_t t6;
    CT_EQ(hp_ir_encode(&c6, 0, &t6), 0);
    CT_EQ(recs[n].h, hash_us(t6.us, t6.once_n));
    key(2, HP_KEY_ACTION_RELEASE);
    advance(g_now + 300);
    /* RC6 at the same address is independent of RC5 */
    n = nrec;
    key(3, HP_KEY_ACTION_PRESS);
    hp_ir_code_t c7 = {.kind = HP_IR_CODE_PROTO, .u.p = {HP_IR_RC6, 5, 12}};
    hp_ir_signal_t t7;
    CT_EQ(hp_ir_encode(&c7, 0, &t7), 0);
    CT_EQ(recs[n].h, hash_us(t7.us, t7.once_n));
}

static void test_raw_default_period_and_pronto_repeat(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"raw\":{\"hz\":38000,\"us\":[9000,4500,560,560]}},"
              "\"2\":{\"type\":\"ir\",\"pronto\":\"0000 006d 0001 0001 0018 0018 0030 0030\"}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    key(1, HP_KEY_ACTION_HOLD);
    advance(t0 + 109);
    CT_EQ(nrec, 1);
    advance(t0 + 110); /* no period known: ~110 ms */
    CT_EQ(nrec, 2);
    CT_EQ(recs[1].n, 4);
    key(1, HP_KEY_ACTION_RELEASE);
    advance(g_now + 300); /* let the line go quiet: a press during a frame is deferred */
    size_t n = nrec;
    key(2, HP_KEY_ACTION_PRESS);
    CT_EQ(recs[n].n, 2);
    CT_EQ(recs[n].us0, 631);
    key(2, HP_KEY_ACTION_HOLD);
    advance(g_now + 110);
    CT_EQ(recs[n + 1].n, 2); /* the Pronto repeat part (0x30 periods), not the once part */
    CT_EQ(recs[n + 1].us0, 1262);
}

static void test_non_key_sends_do_not_repeat(void) {
    setup("{\"format\":2,\"devices\":[{\"id\":\"tv\",\"commands\":{\"ok\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2}}}],"
          "\"activities\":[{\"id\":\"a\",\"steps\":[{\"type\":\"device\",\"device\":\"tv\",\"command\":\"ok\"}]}]}");
    CT_EQ(hp_activity_set(&map, "a", &sinks), 0);
    CT_EQ(nrec, 1);
    key(9, HP_KEY_ACTION_HOLD);
    advance(g_now + 500);
    CT_EQ(nrec, 1);
}

/* A press while the line is busy (SIRC copies pending) is deferred, never interleaved; deferred
 * sends go out in order, and a full queue drops the newest. */
static void noop_cb(void *a, uint32_t n) { (void)a; (void)n; }
static int g_done_rc;
static void done_cb(void *c, int rc) { (void)c; g_done_rc = rc; }

/* A full timer queue drops the extra copies and queued sends; frames are never sent back to back. */
static void test_full_timer_queue_drops_instead_of_back_to_back(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8}"));
    for (int i = 0; i < HP_SCHED_CAP; i++) CT_CHECK(hp_sched_after(&sched, 100000, noop_cb, NULL));
    key(1, HP_KEY_ACTION_PRESS);
    CT_EQ(nrec, 1); /* SIRC wants 3 copies: the two extra are dropped */
    advance(g_now + 500);
    CT_EQ(nrec, 1);
    hp_ir_code_t c;
    memset(&c, 0, sizeof c);
    c.kind = HP_IR_CODE_PROTO;
    c.u.p.proto = HP_IR_NEC;
    c.u.p.addr = 4;
    c.u.p.cmd = 8;
    CT_EQ(hp_irout_send_cb(&out, &ir, &c, 1, 5, 1, done_cb, NULL), 0); /* line free: sent now (0), done not called */
    g_done_rc = 0;
    CT_EQ(hp_irout_send_cb(&out, &ir, &c, 1, 6, 1, done_cb, NULL), 1); /* busy: deferred, no timer to wake it */
    CT_EQ(g_done_rc, -HP_E_BUSY);
    CT_EQ(nrec, 2);
}

static void test_press_deferred_while_line_busy(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8},"
              "\"3\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":9}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    advance(t0 + 10);
    key(2, HP_KEY_ACTION_PRESS); /* mid-burst */
    key(3, HP_KEY_ACTION_PRESS); /* queued behind it */
    advance(t0 + 60);
    CT_EQ(nrec, 2); /* sirc copies 1,2 only */
    advance(t0 + 2000);
    CT_EQ(nrec, 5); /* sirc x3, then both presses in order */
    CT_EQ(recs[0].n, 26);
    CT_EQ(recs[1].n, 26);
    CT_EQ(recs[2].n, 26);
    CT_EQ(recs[3].n, 68);
    CT_EQ(recs[4].n, 68);
    CT_CHECK(recs[3].h != recs[4].h);
    CT_CHECK(recs[4].t - recs[3].t >= 60 + HP_IROUT_GUARD_MS);
    CT_CHECK(recs[3].t - recs[2].t >= 25 + HP_IROUT_GUARD_MS - 1); /* after the last copy + guard */
    CT_EQ(hp_sched_pending(&sched), 0);
    CT_CHECK(hp_irout_wait_ms(&out, g_now) == 0);
    /* an idle line sends at once */
    size_t n = nrec;
    key(2, HP_KEY_ACTION_PRESS);
    CT_EQ(nrec, n + 1);
}

/* a stale line-free time (idle >= 2^31 ms) must not look busy */
static void test_line_stale_after_idle(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8}"));
    key(1, HP_KEY_ACTION_PRESS);
    advance(g_now + 200);
    uint32_t idles[] = {0x80000000u + 1000u, 0xFFFFFFFFu - 499u};
    for (int i = 0; i < 2; i++) {
        g_now += idles[i];
        size_t n = nrec;
        key(2, HP_KEY_ACTION_PRESS);
        CT_EQ(nrec, n + 1);
        advance(g_now + 200);
    }
}

/* ir_send (KEY_SEND) defers while the line is busy; plan/format-1 sends (KEY_NONE) never do */
static void test_send_modes(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21}"));
    hp_ir_code_t c = {.kind = HP_IR_CODE_PROTO, .u.p = {HP_IR_NEC, 4, 8}};
    key(1, HP_KEY_ACTION_PRESS);
    CT_EQ(hp_irout_send(&out, &ir, &c, 0xFF, HP_IROUT_KEY_SEND), 0);
    CT_EQ(nrec, 1); /* deferred behind the SIRC copies */
    CT_CHECK(hp_irout_wait_ms(&out, g_now) >= 1);
    advance(g_now + 2000);
    CT_EQ(nrec, 4);
    CT_EQ(recs[3].n, 68);
    /* queue bound: HP_IROUT_PEND deferred sends, the next one is refused */
    key(1, HP_KEY_ACTION_PRESS);
    for (int i = 0; i < HP_IROUT_PEND; i++) CT_EQ(hp_irout_send(&out, &ir, &c, 0xFF, HP_IROUT_KEY_SEND), 0);
    CT_EQ(hp_irout_send(&out, &ir, &c, 0xFF, HP_IROUT_KEY_SEND), -HP_E_BUSY);
    advance(g_now + 3000);
    CT_EQ(nrec, 4 + 3 + HP_IROUT_PEND);
    nrec = 4;
    key(1, HP_KEY_ACTION_PRESS);
    CT_EQ(hp_irout_send(&out, &ir, &c, 0xFF, HP_IROUT_KEY_NONE), 0);
    CT_EQ(nrec, 6); /* immediate (the caller already waited on hp_irout_wait_ms) */
    advance(g_now + 2000);
    CT_EQ(nrec, 8); /* the remaining SIRC copies */
}

/* Per-command `repeats` (config format 2): N frames one frame period apart, the line is busy for all of
 * them, a deferred press keeps its count, SIRC keeps at least 3, out-of-range values are load errors. */
static void test_repeats_per_command(void) {
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8,\"repeats\":3},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":9},"
              "\"3\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21,\"repeats\":5},"
              "\"4\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":22,\"repeats\":2}"));
    key(1, HP_KEY_ACTION_PRESS);
    uint32_t t0 = g_now;
    CT_EQ(nrec, 1);
    advance(t0 + 400);
    CT_EQ(nrec, 3); /* three full frames, no more */
    CT_EQ(recs[1].n, 68);
    CT_EQ(recs[2].t - recs[1].t, recs[1].t - recs[0].t); /* evenly spaced */
    CT_EQ(recs[1].h, recs[0].h);
    CT_EQ(hp_sched_pending(&sched), 0);
    key(1, HP_KEY_ACTION_RELEASE);
    /* no `repeats`: one frame, exactly as before */
    advance(g_now + 500);
    nrec = 0;
    key(2, HP_KEY_ACTION_PRESS);
    advance(g_now + 500);
    CT_EQ(nrec, 1);
    /* SIRC: repeats above 3 win, below 3 never reduce the three copies */
    advance(g_now + 500);
    nrec = 0;
    key(3, HP_KEY_ACTION_PRESS);
    advance(g_now + 1000);
    CT_EQ(nrec, 5);
    advance(g_now + 500);
    nrec = 0;
    key(4, HP_KEY_ACTION_PRESS);
    advance(g_now + 1000);
    CT_EQ(nrec, 3);
    /* a deferred press keeps its repeat count */
    advance(g_now + 500);
    nrec = 0;
    key(3, HP_KEY_ACTION_PRESS);
    key(1, HP_KEY_ACTION_PRESS); /* the line is busy with the SIRC copies: queued */
    advance(g_now + 3000);
    CT_EQ(nrec, 5 + 3);
    CT_EQ(hp_sched_pending(&sched), 0);
}

static void test_repeats_validation(void) {
    hp_map_t m;
    hp_map_init(&m);
    const char *bad[] = {
        CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8,\"repeats\":0}"),
        CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8,\"repeats\":9}"),
        CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8,\"repeats\":\"2\"}"),
        CFG("\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8,\"repeats\":1.5}"),
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        hp_map_free(&m);
        hp_map_init(&m);
        CT_CHECK(hp_map_load(&m, bad[i], strlen(bad[i])) < 0);
    }
    hp_map_free(&m);
}

/* E27: a deeper deferred queue; every press is its own send (no merging); callbacks see the result. */
static int cb_n, cb_rc;
static void cb(void *c, int rc) { (void)c; cb_n++; cb_rc = rc; }

static void test_pending_queue_keeps_presses_and_reports(void) {
    CT_EQ(HP_IROUT_PEND, 16);
    setup(CFG("\"1\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},"
              "\"2\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":8},"
              "\"3\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":4,\"cmd\":9}"));
    key(1, HP_KEY_ACTION_PRESS);
    advance(g_now + 10);
    for (int i = 0; i < 5; i++) key(2, HP_KEY_ACTION_PRESS); /* five quick taps: five sends */
    key(3, HP_KEY_ACTION_PRESS);
    key(2, HP_KEY_ACTION_PRESS);
    advance(g_now + 3000);
    CT_EQ(nrec, 3 + 7);
    CT_EQ(hp_sched_pending(&sched), 0);
    /* deferred send with a callback: 1 = queued, done(rc) once sent */
    nrec = 0;
    hp_ir_code_t c = {.kind = HP_IR_CODE_PROTO, .u.p = {HP_IR_NEC, 4, 8}};
    key(1, HP_KEY_ACTION_PRESS);
    cb_n = 0;
    CT_EQ(hp_irout_send_cb(&out, &ir, &c, 0xFF, HP_IROUT_KEY_SEND, 1, cb, NULL), 1);
    CT_EQ(cb_n, 0);
    advance(g_now + 2000);
    CT_EQ(cb_n, 1);
    CT_EQ(cb_rc, 0);
    CT_EQ(hp_irout_send_cb(&out, &ir, &c, 0xFF, HP_IROUT_KEY_SEND, 1, cb, NULL), 0); /* idle line: sent now, no callback */
    CT_EQ(cb_n, 1);
}

int main(void) {
    CT_RUN(test_nec_hold_repeat_and_cap);
    CT_RUN(test_periodic_hold_extends_cap);
    CT_RUN(test_release_stops_repeat);
    CT_RUN(test_new_press_ends_hold);
    CT_RUN(test_sirc_three_frames);
    CT_RUN(test_samsung_repeats_full_frame);
    CT_RUN(test_full_timer_queue_drops_instead_of_back_to_back);
    CT_RUN(test_rc5_toggle);
    CT_RUN(test_raw_default_period_and_pronto_repeat);
    CT_RUN(test_non_key_sends_do_not_repeat);
    CT_RUN(test_press_deferred_while_line_busy);
    CT_RUN(test_line_stale_after_idle);
    CT_RUN(test_send_modes);
    CT_RUN(test_pending_queue_keeps_presses_and_reports);
    CT_RUN(test_repeats_per_command);
    CT_RUN(test_repeats_validation);
    hp_map_free(&map);
    return CT_DONE();
}
