#define _POSIX_C_SOURCE 200809L
/* Activity state machine: transition tables, settle delays, interruption, persistence (fake clock). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/engine/activity.h"
#include "hub/hp_errors.h"
#include "hub/net/hublog.h"

static uint32_t g_now;
static uint32_t clk(void *c) { (void)c; return g_now; }

typedef struct {
    uint32_t t, h, dur_ms;
} rec_t;
static rec_t recs[128];
static size_t nrec;

static uint32_t hash_us(const uint16_t *us, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ us[i]) * 16777619u;
    return h;
}

static int rec_send(void *self, uint8_t port, const hp_ir_signal_t *sig) {
    (void)self; (void)port;
    CT_CHECK(nrec < 128);
    uint32_t us = 0;
    for (size_t i = 0; i < sig->once_n; i++) us += sig->us[i];
    recs[nrec].t = g_now;
    recs[nrec].h = hash_us(sig->us, sig->once_n);
    recs[nrec].dur_ms = (us + 999) / 1000;
    nrec++;
    return 0;
}

static uint32_t fid(hp_ir_proto_t p, uint32_t addr, uint32_t cmd) {
    hp_ir_code_t c;
    hp_ir_signal_t s;
    memset(&c, 0, sizeof c);
    c.kind = HP_IR_CODE_PROTO;
    c.u.p.proto = p;
    c.u.p.addr = addr;
    c.u.p.cmd = cmd;
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    return hash_us(s.us, s.once_n);
}
#define NEC(a, c) fid(HP_IR_NEC, a, c)
#define SIRC(a, c) fid(HP_IR_SIRC12, a, c)

static hp_ir_sink ir = {NULL, rec_send};
static hp_sinks_t sinks = {&ir, NULL, NULL};
static hp_sched_t sched;
static hp_irout_t out;
static hp_map_t map;
static hp_actsm_t sm;

typedef struct {
    int kind, idx;
    bool off;
} ev_t;
static ev_t evs[256];
static size_t nev;
static void notify(void *c, hp_actsm_evt_t e, int idx) {
    (void)c;
    if (nev < 256) evs[nev++] = (ev_t){e, idx, hp_actsm_off_press(&sm)};
}
static size_t count_off_ev(void) {
    size_t n = 0;
    for (size_t i = 0; i < nev; i++) n += evs[i].kind == HP_ACTSM_EVT_ACTIVITY && evs[i].off;
    return n;
}
static size_t count_ev(int kind) {
    size_t n = 0;
    for (size_t i = 0; i < nev; i++) n += evs[i].kind == kind;
    return n;
}

static const char *CFG =
    "{\"format\":2,\"devices\":["
    "{\"id\":\"tv\",\"settle_ms\":300,"
    " \"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":1},\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2}},"
    " \"inputs\":{\"hdmi1\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":10},\"hdmi2\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":11}},"
    " \"commands\":{\"vol_up\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":20}}},"
    "{\"id\":\"amp\",\"settle_ms\":200,\"power\":{\"toggle\":{\"proto\":\"nec\",\"addr\":9,\"cmd\":1}},"
    " \"inputs\":{\"opt\":{\"proto\":\"nec\",\"addr\":9,\"cmd\":5}}},"
    "{\"id\":\"bd\",\"power\":{\"on\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},\"off\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":22}}}],"
    "\"activities\":["
    "{\"id\":\"watch\",\"name\":\"watch\",\"devices\":[{\"device\":\"tv\",\"input\":\"hdmi1\"},{\"device\":\"amp\",\"input\":\"opt\"}],"
    " \"steps\":[{\"type\":\"device\",\"device\":\"tv\",\"command\":\"vol_up\"}],"
    " \"off\":[{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":7,\"cmd\":1}]},"
    "{\"id\":\"movie\",\"devices\":[{\"device\":\"tv\",\"input\":\"hdmi2\"},{\"device\":\"bd\"}]},"
    "{\"id\":\"music\",\"devices\":[{\"device\":\"amp\",\"input\":\"opt\"}]}],"
    "\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"watch\"},\"3\":{\"type\":\"activity\",\"activity\":\"off\"},"
    "\"4\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":5,\"cmd\":5}}}";

static void setup_cfg(const char *cfg) {
    hp_map_free(&map);
    g_now = 10000;
    nrec = nev = 0;
    hp_sched_init(&sched, clk, NULL);
    hp_irout_init(&out, &sched);
    hp_map_init(&map);
    hp_map_set_out(&map, &out);
    int r = hp_map_load(&map, cfg, strlen(cfg));
    if (r < 0) fprintf(stderr, "load: %s\n", hp_map_last_error(&map));
    CT_EQ(r, 0);
    memset(&sm, 0, sizeof sm);
    hp_actsm_init(&sm, &map, &sched, &sinks, notify, NULL);
    hp_actsm_reloaded(&sm);
    nev = 0;
}
static void setup(void) { setup_cfg(CFG); }

static void advance(uint32_t to) {
    while (g_now < to) {
        g_now++;
        hp_sched_run(&sched, g_now);
    }
}
static void settle_all(void) { advance(g_now + 5000); }

static void expect(size_t n, const uint32_t *ids, int line) {
    if (nrec != n) {
        fprintf(stderr, "line %d: %zu frames, want %zu\n", line, nrec, n);
        ct_failures++;
        return;
    }
    for (size_t i = 0; i < n; i++)
        if (recs[i].h != ids[i]) {
            fprintf(stderr, "line %d: frame %zu differs\n", line, i);
            ct_failures++;
        }
}
#define EXPECT(...) \
    do { const uint32_t _ids[] = {__VA_ARGS__}; expect(sizeof _ids / sizeof _ids[0], _ids, __LINE__); } while (0)

/* no two frames overlap: each starts at least its predecessor's duration (+ guard between plan
 * steps; SIRC copies are spaced by the irout period, which also exceeds the frame time) later */
static void check_no_overlap(void) {
    for (size_t i = 1; i < nrec; i++) CT_CHECK(recs[i].t - recs[i - 1].t >= recs[i - 1].dur_ms);
}
/* between different steps (not SIRC copies) the guard applies */
static void check_guard(void) {
    for (size_t i = 1; i < nrec; i++)
        if (recs[i].h != recs[i - 1].h) CT_CHECK(recs[i].t - recs[i - 1].t >= recs[i - 1].dur_ms + HP_ACT_GUARD_MS);
}

static const char *pw(size_t i) { return hp_actsm_device(&sm, i)->power ? "on" : "off"; }
static const char *in(size_t i) { return hp_actsm_device(&sm, i)->input; }

static void test_start_from_off(void) {
    setup();
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    CT_EQ(hp_actsm_reported(&sm), -1); /* mid-transition: last completed state */
    CT_EQ(map.current, 0);
    CT_CHECK(hp_actsm_busy(&sm));
    settle_all();
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_actsm_reported(&sm), 0);
    EXPECT(NEC(4, 1), NEC(9, 1), NEC(4, 10), NEC(9, 5), NEC(4, 20));
    /* input waits for the device's settle_ms after its power-on */
    CT_CHECK(recs[2].t - recs[0].t >= 300);
    CT_CHECK(recs[3].t - recs[1].t >= 200);
    check_no_overlap();
    check_guard();
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1"));
    CT_CHECK(!strcmp(pw(1), "on") && !strcmp(in(1), "opt"));
    CT_CHECK(!strcmp(pw(2), "off"));
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1); /* exactly once, at completion */
    CT_EQ(evs[nev - 1].kind, HP_ACTSM_EVT_ACTIVITY);
    CT_EQ(evs[nev - 1].idx, 0);
    CT_EQ(count_ev(HP_ACTSM_EVT_DEVICE), 4); /* tv on, amp on, tv input, amp input */
    CT_EQ(hp_sched_pending(&sched), 0);
}

static void test_a_to_b_shared_devices(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = nev = 0;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    settle_all();
    /* tv stays on: only its input changes. amp (toggle) is off by toggle, bd turns on (SIRC x3). */
    EXPECT(NEC(9, 1), SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 11));
    check_no_overlap();
    check_guard();
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi2"));
    CT_CHECK(!strcmp(pw(1), "off") && !strcmp(in(1), ""));
    CT_CHECK(!strcmp(pw(2), "on"));
    CT_EQ(hp_actsm_reported(&sm), 1);
}

static void test_toggle_never_sent_when_in_wanted_state(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = 0;
    CT_EQ(hp_actsm_start(&sm, "music"), 2);
    settle_all();
    /* amp already on with input opt: nothing to do for it; tv (unused) goes off */
    EXPECT(NEC(4, 2));
    nrec = 0;
    CT_EQ(hp_actsm_start(&sm, "music"), 2); /* same activity again: resync, but amp's toggle power is never resent */
    settle_all();
    EXPECT(NEC(9, 5)); /* amp input only: no amp toggle, no off frames to the unused tv/bd */
}

static void test_repress_resyncs_stale_discrete_state(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = nev = 0;
    /* the real TV was switched off behind the hub's back: tracked state still says on/hdmi1 */
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    settle_all();
    /* tv PWR_ON + input resent, amp (toggle) power skipped but its discrete input resent, then the step; no off to bd */
    EXPECT(NEC(4, 1), NEC(4, 10), NEC(9, 5), NEC(4, 20));
    CT_CHECK(recs[1].t - recs[0].t >= 300); /* the forced power-on settles before the forced input (a booting device ignores it) */
    CT_EQ(count_ev(HP_ACTSM_EVT_DEVICE), 0); /* ... nor clear the tracked input */
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1"));
    CT_EQ(hp_actsm_reported(&sm), 0);
    /* resync is one-shot: the next switch to another activity skips by tracked state again */
    nrec = 0;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    settle_all();
    EXPECT(NEC(9, 1), SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 11));
}

static void test_switch_still_skips_tracked(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = 0;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    settle_all();
    EXPECT(NEC(9, 1), SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 11)); /* tv power on not resent */
}

static void test_unknown_input_is_sent(void) {
    setup();
    CT_EQ(hp_actsm_set_device(&sm, "tv", 1, ""), 0); /* tv is on, input unknown */
    CT_EQ(hp_actsm_set_device(&sm, "amp", 1, "opt"), 0);
    CT_EQ(nrec, 0); /* corrections never send IR */
    hp_actsm_start(&sm, "watch");
    settle_all();
    EXPECT(NEC(4, 10), NEC(4, 20)); /* no power frames; only the unknown tv input + step */
}

static void test_off(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = nev = 0;
    CT_EQ(hp_actsm_off(&sm), 0);
    settle_all();
    EXPECT(NEC(4, 2), NEC(9, 1), NEC(7, 1)); /* tv off, amp toggle, then watch's off steps */
    check_guard();
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_EQ(map.current, -1);
    CT_CHECK(!strcmp(pw(0), "off") && !strcmp(in(0), ""));
    CT_CHECK(!strcmp(pw(1), "off"));
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    /* D21: nothing tracked on: Off still resends the discrete off of the last activity's devices (amp is a toggle: skipped) */
    nrec = 0;
    CT_EQ(hp_actsm_off(&sm), 0);
    settle_all();
    EXPECT(NEC(4, 2));
    CT_EQ(hp_actsm_reported(&sm), -1);
}

static void test_off_with_unknown_state(void) {
    setup();
    CT_EQ(hp_actsm_off(&sm), 0); /* no activity known: every discrete-off device (tv, bd), never the toggle amp */
    settle_all();
    EXPECT(NEC(4, 2), SIRC(1, 22), SIRC(1, 22), SIRC(1, 22));
    nrec = 0;
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = 0;
    hp_actsm_set_device(&sm, "tv", 0, NULL); /* partly stale: tv tracked off but really on; amp tracked on */
    CT_EQ(hp_actsm_off(&sm), 0);
    settle_all();
    EXPECT(NEC(4, 2), NEC(9, 1), NEC(7, 1)); /* tv forced discrete, amp toggle (tracked on), watch's off steps; no bd */
}

static void test_interrupted_transition(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    advance(g_now + 20); /* tv on went out at t0; amp toggle not yet (guard) */
    CT_EQ(nrec, 1);
    uint32_t t_tv_on = recs[0].t;
    CT_EQ(hp_actsm_start(&sm, "music"), 2); /* interrupt */
    CT_EQ(map.current, 2);
    settle_all();
    /* tv (on, unused by music) is powered off, amp toggled on, input set; no watch input/step */
    EXPECT(NEC(4, 1), NEC(4, 2), NEC(9, 1), NEC(9, 5));
    check_no_overlap();
    check_guard();
    CT_CHECK(recs[1].t - t_tv_on >= 20); /* bus gap persists across plans */
    CT_CHECK(!strcmp(pw(0), "off") && !strcmp(pw(1), "on") && !strcmp(in(1), "opt"));
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    CT_EQ(evs[nev - 1].idx, 2); /* watch was never reported */
    CT_EQ(hp_sched_pending(&sched), 0);
}

static void test_interrupt_keeps_settle(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    advance(g_now + 20);
    uint32_t t_tv_on = recs[0].t;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1); /* tv wanted again (hdmi2): input must wait for settle */
    settle_all();
    EXPECT(NEC(4, 1), SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 11));
    CT_CHECK(recs[4].t - t_tv_on >= 300);
}

static void test_off_during_transition(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    advance(g_now + 200); /* tv on, amp on sent */
    CT_EQ(nrec, 2);
    CT_EQ(hp_actsm_off(&sm), 0); /* current = watch (target) */
    settle_all();
    EXPECT(NEC(4, 1), NEC(9, 1), NEC(4, 2), NEC(9, 1), NEC(7, 1));
    check_guard();
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1); /* the interrupted start never completed; off did */
    CT_EQ(evs[nev - 1].idx, -1);
    CT_CHECK(!strcmp(pw(0), "off") && !strcmp(pw(1), "off"));
}

static void test_set_device_during_transition_skips_redundant(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    advance(g_now + 20);
    CT_EQ(hp_actsm_set_device(&sm, "amp", 1, "opt"), 0); /* user says amp is already on/opt */
    settle_all();
    EXPECT(NEC(4, 1), NEC(4, 10), NEC(4, 20));
}

/* The silent drops now say so: hp_log lines in a captured file. */
static int cap_begin(hp_hublog_t *l, char *path) {
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    CT_EQ(hp_hublog_init(l, NULL, NULL), 0);
    l->out_fd = fd;
    hp_hublog_install(l);
    return fd;
}

static void cap_end(int fd, const char *path, char *buf, size_t n) {
    ssize_t r = pread(fd, buf, n - 1, 0);
    buf[r > 0 ? r : 0] = 0;
    hp_hublog_install(NULL);
    close(fd);
    unlink(path);
}

static void test_drops_are_logged(void) {
    static hp_hublog_t l;
    char path[] = "/tmp/actlogXXXXXX", buf[2048];
    setup();
    int fd = cap_begin(&l, path);
    hp_actsm_start(&sm, "watch");
    advance(g_now + 20);
    CT_EQ(hp_actsm_set_device(&sm, "amp", 1, "opt"), 0); /* tracked as on: the plan skips amp's power-on */
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* busy: dropped */
    settle_all();
    CT_EQ(hp_key_handle(&map, &sinks, 1, 3, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* off key */
    settle_all();
    CT_EQ(hp_key_handle(&map, &sinks, 1, 3, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* off again: nothing is on */
    settle_all();
    cap_end(fd, path, buf, sizeof buf);
    CT_CHECK(strstr(buf, "hub: step skipped, tracked state says amp is already on\n"));
    CT_CHECK(strstr(buf, "hub: key 4 queued: activity change in progress\n"));
    CT_CHECK(!strstr(buf, "Off ignored"));
}

static void test_set_device_errors(void) {
    setup();
    CT_EQ(hp_actsm_set_device(&sm, "ghost", 1, NULL), -HP_E_INVAL);
    CT_EQ(hp_actsm_set_device(&sm, "tv", -1, "nope"), -HP_E_INVAL);
    CT_EQ(hp_actsm_set_device(&sm, "tv", -1, "hdmi2"), 0);
    CT_CHECK(!strcmp(in(0), "hdmi2") && !strcmp(pw(0), "off"));
    CT_EQ(count_ev(HP_ACTSM_EVT_DEVICE), 1);
    CT_EQ(hp_actsm_set_device(&sm, "tv", -1, "hdmi2"), 0); /* no change: no event */
    CT_EQ(count_ev(HP_ACTSM_EVT_DEVICE), 1);
    CT_EQ(hp_actsm_start(&sm, "nope"), -HP_E_INVAL);
}

static int emitted_layer = -9;
static void layer_emit(void *ctx, uint8_t remote, uint8_t key, hp_key_action_t action) {
    (void)ctx; (void)remote; (void)key; (void)action;
    emitted_layer = map.key_layer;
}

/* the layer a key event reports is the one the key was resolved in, not the one its own action switches to */
static void test_emitted_key_layer_is_the_lookup_layer(void) {
    setup();
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, layer_emit, NULL), 0);
    CT_EQ(emitted_layer, -1); /* nothing running */
    CT_EQ(hp_key_handle(&map, &sinks, 1, 1, HP_KEY_ACTION_PRESS, layer_emit, NULL), 0); /* starts activity 0 */
    CT_EQ(map.current, 0);
    CT_EQ(emitted_layer, -1); /* looked up before it switched */
    settle_all();
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, layer_emit, NULL), 0);
    CT_EQ(emitted_layer, 0);
}

static void test_key_driven_and_busy_keys(void) {
    setup();
    CT_EQ(hp_key_handle(&map, &sinks, 1, 1, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* activity key */
    CT_EQ(map.current, 0);
    CT_CHECK(hp_actsm_busy(&sm));
    advance(g_now + 20);
    size_t n = nrec;
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* plain IR key: queued */
    CT_EQ(nrec, n);
    settle_all();
    EXPECT(NEC(4, 1), NEC(9, 1), NEC(4, 10), NEC(9, 5), NEC(4, 20), NEC(5, 5)); /* replayed once the transition finished */
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* idle again */
    CT_EQ(nrec, 7);
    CT_EQ(hp_key_handle(&map, &sinks, 1, 3, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* off key */
    settle_all();
    CT_EQ(hp_actsm_reported(&sm), -1);
}

/* A Bluetooth key a physical press holds down must be let go when the activity changes or powers off. */
static int bt_n, bt_downs;
static int rec_bt(void *self, uint8_t host, uint8_t page, uint16_t usage, bool down) {
    (void)self; (void)host; (void)page; (void)usage;
    bt_n++;
    bt_downs += down;
    return 0;
}
static hp_bt_sink btsink = {NULL, rec_bt};

static const char *BTACT =
    "{\"format\":2,\"devices\":[{\"id\":\"tv\",\"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":1},"
    "\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2}}},"
    "{\"id\":\"shield\",\"commands\":{\"Home\":{\"type\":\"bt\",\"page\":12,\"usage\":547,\"host\":3}}},"
    "{\"id\":\"box\",\"commands\":{\"Vol\":{\"type\":\"bt\",\"usage\":233,\"host\":7}}}],"
    "\"activities\":[{\"id\":\"w\",\"devices\":[{\"device\":\"tv\"},{\"device\":\"shield\"}]},"
    "{\"id\":\"x\",\"devices\":[{\"device\":\"tv\"},{\"device\":\"box\"}]}],"
    "\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"w\"},\"2\":{\"type\":\"activity\",\"activity\":\"x\"},"
    "\"3\":{\"type\":\"activity\",\"activity\":\"off\"},\"6\":{\"type\":\"bt\",\"usage\":233,\"host\":3}}}";

static void test_bt_key_released_on_switch_and_off(void) {
    sinks.bt = &btsink;
    for (int path = 0; path < 2; path++) { /* 0: switch activity, 1: power off */
        setup_cfg(BTACT);
        bt_n = bt_downs = 0;
        CT_EQ(hp_actsm_start(&sm, "w"), 0);
        settle_all();
        CT_EQ(hp_key_handle(&map, &sinks, 1, 6, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* a remote key held down */
        CT_EQ(bt_n, 1);
        CT_EQ(bt_downs, 1);
        CT_CHECK(map.bt_held.active);
        if (path == 0) CT_EQ(hp_actsm_start(&sm, "x"), 1);
        else CT_EQ(hp_actsm_off(&sm), 0);
        CT_EQ(bt_n, 2); /* the key up went out before the transition */
        CT_EQ(bt_downs, 1);
        CT_CHECK(!map.bt_held.active);
        settle_all();
        CT_EQ(bt_n, 2); /* and only once */
    }
    sinks.bt = NULL;
}

static void test_start_event_and_activity_bt_hosts(void) {
    setup_cfg(BTACT);
    uint8_t hosts[4];
    CT_EQ(hp_map_activity_bt_hosts(&map, 0, hosts, 4), 1); /* w uses the shield (host 3) */
    CT_EQ(hosts[0], 3);
    CT_EQ(hp_map_activity_bt_hosts(&map, 1, hosts, 4), 1); /* x uses the box (host 7) */
    CT_EQ(hosts[0], 7);
    CT_EQ(hp_map_activity_bt_hosts(&map, 5, hosts, 4), 0);
    CT_EQ(hp_actsm_start(&sm, "x"), 1);
    CT_EQ(count_ev(HP_ACTSM_EVT_START), 1);
    CT_EQ(evs[0].kind, HP_ACTSM_EVT_START); /* before anything is sent: the host connects while the IR steps run */
    CT_EQ(evs[0].idx, 1);
    CT_EQ(hp_actsm_start(&sm, "nope"), -HP_E_INVAL);
    CT_EQ(count_ev(HP_ACTSM_EVT_START), 1);
}

static void test_format1_unchanged(void) {
    setup_cfg("{\"activities\":[{\"name\":\"TV\",\"steps\":[{\"type\":\"ir\",\"port_mask\":255,\"pronto\":[0,109,1,0,10,20]}],"
              "\"off\":[{\"type\":\"ir\",\"port_mask\":255,\"pronto\":[0,109,1,0,30,40]}]}]}");
    CT_EQ(hp_actsm_start(&sm, "TV"), 0);
    CT_EQ(nrec, 1); /* immediate, no timers */
    CT_EQ(hp_sched_pending(&sched), 0);
    CT_EQ(hp_actsm_reported(&sm), 0);
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    CT_EQ(hp_actsm_off(&sm), 0);
    CT_EQ(nrec, 2);
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_EQ(hp_actsm_off(&sm), -HP_E_INVAL);
}

static void test_reload_keeps_state_by_id(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    CT_EQ(hp_map_load(&map, CFG, strlen(CFG)), 0);
    hp_actsm_reloaded(&sm);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1"));
    CT_EQ(hp_actsm_reported(&sm), 0);
    hp_actsm_start(&sm, "movie");
    CT_CHECK(hp_actsm_busy(&sm));
    CT_EQ(hp_map_load(&map, CFG, strlen(CFG)), 0); /* D24: an unchanged config leaves the transition alone */
    hp_actsm_reloaded(&sm);
    CT_CHECK(hp_actsm_busy(&sm));
    settle_all();
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_actsm_reported(&sm), 1);
    CT_EQ(hp_sched_pending(&sched), 0);
}

/* ---------------------------------------------------------- persistence */

static char path[64];

static void tmp_path(void) {
    snprintf(path, sizeof path, "/tmp/hp_state_%ld.bin", (long)getpid());
    unlink(path);
}

static void test_state_roundtrip(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    hp_actsm_start(&sm, "watch");
    settle_all();
    CT_EQ(hp_actsm_flush_state(&sm), 0);
    /* fresh engine over the same config */
    setup();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_load_state(&sm), 0);
    CT_EQ(hp_actsm_reported(&sm), 0);
    CT_EQ(map.current, 0);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1"));
    CT_CHECK(!strcmp(pw(1), "on") && !strcmp(in(1), "opt"));
    CT_CHECK(!strcmp(pw(2), "off"));
    /* restored state drives the next transition: tv/amp already set up */
    nrec = 0;
    hp_actsm_start(&sm, "music");
    settle_all();
    EXPECT(NEC(4, 2));
    unlink(path);
}

static void test_state_off_roundtrip(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    hp_actsm_set_device(&sm, "tv", 1, "hdmi2");
    CT_EQ(hp_actsm_flush_state(&sm), 0);
    setup();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_load_state(&sm), 0);
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_EQ(map.current, -1);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi2"));
    unlink(path);
}

static void write_raw(const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    CT_CHECK(f != NULL);
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

static void test_state_bad_files(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_load_state(&sm), -HP_E_NOENT);
    hp_actsm_start(&sm, "watch");
    settle_all();
    hp_actsm_flush_state(&sm);
    FILE *f = fopen(path, "rb");
    uint8_t buf[512];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    CT_CHECK(n > 20 && !memcmp(buf, "HPST", 4) && buf[4] == HP_ACT_STATE_VER);

    uint8_t bad[512];
    struct { const char *what; size_t len; int pos; } cases[] = {
        {"truncated", n - 3, -1}, {"empty", 0, -1}, {"bit flip", n, 12}, {"bad magic", n, 0}, {"bad version", n, 4},
        {"hash mismatch", n, 5},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memcpy(bad, buf, n);
        if (cases[i].pos >= 0) bad[cases[i].pos] ^= 0x40;
        if (i == 5) { /* a valid CRC over a different config hash */
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t k = 0; k < n - 4; k++) {
                crc ^= bad[k];
                for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            crc = ~crc;
            bad[n - 4] = (uint8_t)(crc >> 24); bad[n - 3] = (uint8_t)(crc >> 16);
            bad[n - 2] = (uint8_t)(crc >> 8); bad[n - 1] = (uint8_t)crc;
        }
        write_raw(bad, cases[i].len);
        setup();
        hp_actsm_set_state_path(&sm, path);
        int r = hp_actsm_load_state(&sm);
        if (i == 5) { /* D23: another config hash is restored per id */
            CT_EQ(r, 0);
            CT_EQ(hp_actsm_reported(&sm), 0);
            CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1"));
            continue;
        }
        if (r != -HP_E_INVAL) fprintf(stderr, "case %s: rc %d\n", cases[i].what, r);
        CT_EQ(r, -HP_E_INVAL);
        CT_EQ(hp_actsm_reported(&sm), -1); /* all off/unknown, no crash */
        CT_CHECK(!strcmp(pw(0), "off") && !strcmp(in(0), ""));
    }
    unlink(path);
}

static void test_state_debounce(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    hp_actsm_set_device(&sm, "tv", 1, NULL);
    CT_EQ(sm.writes, 1); /* first change writes at once */
    for (int i = 0; i < 20; i++) {
        hp_actsm_set_device(&sm, "tv", -1, i & 1 ? "hdmi1" : "hdmi2"); /* burst: coalesced */
        advance(g_now + 50);
    }
    CT_EQ(sm.writes, 1);
    advance(g_now + HP_ACT_PERSIST_MS);
    CT_EQ(sm.writes, 2); /* one trailing write with the final state */
    advance(g_now + 10000);
    CT_EQ(sm.writes, 2); /* nothing dirty: no more writes */
    CT_EQ(hp_sched_pending(&sched), 0);
    hp_actsm_set_device(&sm, "tv", 0, NULL);
    CT_EQ(sm.writes, 3); /* idle for long enough: leading write */
    hp_actsm_set_device(&sm, "tv", 1, NULL);
    CT_EQ(sm.writes, 3);
    CT_EQ(hp_actsm_flush_state(&sm), 0); /* clean shutdown flushes pending state */
    CT_EQ(sm.writes, 4);
    setup();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_load_state(&sm), 0);
    CT_CHECK(!strcmp(pw(0), "on"));
    unlink(path);
}


/* A hub idle for >= 2^31 ms must not see stale timestamps (bus_free, settle_until, last_write) as
 * being in the future. */
static void idle_clock_case(uint32_t idle) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    settle_all();
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_sched_pending(&sched), 0);
    g_now += idle; /* the hub sat idle: no timers, the clock just moved on */
    size_t n0 = nrec;
    unsigned w0 = sm.writes;
    uint32_t t0 = g_now;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    CT_EQ(nrec, n0 + 1); /* first step (amp off) goes out at once, not ~24 days from now */
    CT_EQ(sm.writes, w0); /* no synchronous write while the transition runs ... */
    settle_all();
    CT_EQ(sm.writes, w0 + 1); /* ... one at its end */
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_actsm_reported(&sm), 1);
    CT_EQ(nrec, n0 + 5); /* amp off, bd on (x3 SIRC), tv input */
    CT_CHECK(recs[nrec - 1].t - t0 < 1000); /* stale tv settle_until did not delay the input select */
    size_t n1 = nrec;
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(nrec, n1 + 1); /* keys are not dropped */
    unlink(path);
}

static void test_idle_clock_wrap(void) {
    idle_clock_case(0x80000000u + 1000u);
    idle_clock_case(0xFFFFFFFFu - 499u); /* 2^32 - 500 */
}

/* A reload while a transition runs must not report (or persist) the unfinished target. */
static const char *CFG_B = /* CFG with a trailing blank: another hash, same content */
    "{\"format\":2,\"devices\":["
    "{\"id\":\"tv\",\"settle_ms\":300,"
    " \"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":1},\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2}},"
    " \"inputs\":{\"hdmi1\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":10},\"hdmi2\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":11}},"
    " \"commands\":{\"vol_up\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":20}}},"
    "{\"id\":\"amp\",\"settle_ms\":200,\"power\":{\"toggle\":{\"proto\":\"nec\",\"addr\":9,\"cmd\":1}},"
    " \"inputs\":{\"opt\":{\"proto\":\"nec\",\"addr\":9,\"cmd\":5}}},"
    "{\"id\":\"bd\",\"power\":{\"on\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},\"off\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":22}}}],"
    "\"activities\":["
    "{\"id\":\"watch\",\"name\":\"watch\",\"devices\":[{\"device\":\"tv\",\"input\":\"hdmi1\"},{\"device\":\"amp\",\"input\":\"opt\"}],"
    " \"steps\":[{\"type\":\"device\",\"device\":\"tv\",\"command\":\"vol_up\"}],"
    " \"off\":[{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":7,\"cmd\":1}]},"
    "{\"id\":\"movie\",\"devices\":[{\"device\":\"tv\",\"input\":\"hdmi2\"},{\"device\":\"bd\"}]},"
    "{\"id\":\"music\",\"devices\":[{\"device\":\"amp\",\"input\":\"opt\"}]}],"
    "\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"watch\"},\"3\":{\"type\":\"activity\",\"activity\":\"off\"},"
    "\"4\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":5,\"cmd\":5}}} ";

/* D24: a changed config mid-transition restarts the plan for the surviving target; it is reported only once done. */
static void test_reload_mid_transition_restarts(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    settle_all();
    CT_EQ(hp_actsm_reported(&sm), 0);
    advance(g_now + 3000);
    hp_actsm_start(&sm, "movie");
    advance(g_now + 20);
    CT_CHECK(hp_actsm_busy(&sm));
    nev = 0;
    CT_EQ(hp_map_load(&map, CFG_B, strlen(CFG_B)), 0);
    hp_actsm_reloaded(&sm);
    CT_CHECK(hp_actsm_busy(&sm)); /* restarted, not cancelled */
    CT_EQ(hp_actsm_reported(&sm), 0); /* still the last completed activity */
    CT_EQ(map.current, 1);
    for (size_t i = 0; i < nev; i++) CT_CHECK(!(evs[i].kind == HP_ACTSM_EVT_ACTIVITY && evs[i].idx == 1));
    settle_all();
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_actsm_reported(&sm), 1);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi2") && !strcmp(pw(2), "on") && !strcmp(pw(1), "off"));
    CT_EQ(hp_sched_pending(&sched), 0);
    /* a target the new config lacks is cancelled; the reported activity that vanished becomes none */
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    hp_actsm_start(&sm, "music");
    advance(g_now + 20);
    const char *nw = "{\"format\":2,\"devices\":[{\"id\":\"tv\"}],\"activities\":[{\"id\":\"movie\",\"devices\":[{\"device\":\"tv\"}]}]}";
    CT_EQ(hp_map_load(&map, nw, strlen(nw)), 0);
    hp_actsm_reloaded(&sm);
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_EQ(map.current, -1);
    CT_EQ(hp_sched_pending(&sched), 0);
    /* an Off in flight restarts as an Off */
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    hp_actsm_off(&sm);
    CT_CHECK(hp_actsm_busy(&sm));
    CT_EQ(hp_map_load(&map, CFG_B, strlen(CFG_B)), 0);
    hp_actsm_reloaded(&sm);
    settle_all();
    CT_EQ(hp_actsm_reported(&sm), -1);
    CT_CHECK(!strcmp(pw(0), "off") && !strcmp(pw(1), "off"));
    unlink(path);
}


/* ------------------------------------------------- IR line interleaving */

static const char *CFG3 =
    "{\"format\":2,\"devices\":["
    "{\"id\":\"tv\",\"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":1},\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2}}},"
    "{\"id\":\"bd\",\"power\":{\"on\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":21},\"off\":{\"proto\":\"sirc12\",\"addr\":1,\"cmd\":22}}}],"
    "\"activities\":[{\"id\":\"a\",\"devices\":[{\"device\":\"tv\"}]},{\"id\":\"b\",\"devices\":[{\"device\":\"bd\"}]}],"
    "\"keys\":{\"5\":{\"type\":\"ir\",\"proto\":\"sirc12\",\"addr\":1,\"cmd\":2},"
    "\"6\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":5,\"cmd\":5}}}";

static void press(uint8_t k) { CT_EQ(hp_key_handle(&map, &sinks, 1, k, HP_KEY_ACTION_PRESS, NULL, NULL), 0); }

/* no frame may start before its predecessor ended */
static void check_frames_dont_overlap(void) {
    for (size_t i = 1; i < nrec; i++)
        if (recs[i].t - recs[i - 1].t < recs[i - 1].dur_ms) {
            fprintf(stderr, "frames %zu and %zu overlap (%u ms apart, %u ms long)\n", i - 1, i,
                    (unsigned)(recs[i].t - recs[i - 1].t), (unsigned)recs[i - 1].dur_ms);
            ct_failures++;
        }
}

/* (A) copies 2 and 3 of a SIRC press are pending when a plan starts */
static void test_plan_waits_for_sirc_copies(void) {
    setup_cfg(CFG3);
    press(5);
    uint32_t t0 = g_now;
    advance(t0 + 10);
    CT_EQ(hp_actsm_start(&sm, "a"), 0);
    settle_all();
    EXPECT(SIRC(1, 2), SIRC(1, 2), SIRC(1, 2), NEC(4, 1));
    CT_CHECK(recs[3].t - t0 >= 90 + recs[2].dur_ms + HP_ACT_GUARD_MS);
    check_frames_dont_overlap();
}

/* (B) finish() hands off the last step; a press inside that frame's busy time is deferred */
static void test_press_after_finish_deferred(void) {
    setup_cfg(CFG3);
    CT_EQ(hp_actsm_start(&sm, "b"), 1);
    CT_CHECK(!hp_actsm_busy(&sm)); /* the only step went out, plan finished */
    uint32_t t0 = recs[0].t;
    advance(g_now + 10);
    press(6);
    settle_all();
    EXPECT(SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(5, 5));
    CT_CHECK(recs[3].t - t0 >= 90 + recs[2].dur_ms + HP_ACT_GUARD_MS - 1);
    check_frames_dont_overlap();
}

/* a hold repeat racing a plan start (HA-issued, so no key press cancels the hold) */
static void test_hold_repeat_vs_plan_start(void) {
    setup_cfg(CFG3);
    press(6);
    CT_EQ(hp_key_handle(&map, &sinks, 1, 6, HP_KEY_ACTION_HOLD, NULL, NULL), 0);
    advance(g_now + 105); /* first repeat due at +110 */
    CT_EQ(hp_actsm_start(&sm, "a"), 0);
    advance(g_now + 600);
    check_frames_dont_overlap();
    size_t n = nrec;
    advance(g_now + 1500);
    CT_EQ(nrec, n); /* the hold ended with the new plan: no repeats keep firing */
    CT_EQ(recs[nrec - 1].h, NEC(4, 1));
}

/* Renaming an activity (same id) keeps it current: identity is the id. */
static void test_reload_rename_keeps_current(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nev = 0;
    char *cfg = strdup(CFG);
    char *p = strstr(cfg, "\"name\":\"watch\"");
    CT_CHECK(p != NULL);
    memcpy(p, "\"name\":\"WATCH\"", 14);
    CT_EQ(hp_map_load(&map, cfg, strlen(cfg)), 0);
    CT_EQ(map.current, 0);
    hp_actsm_reloaded(&sm);
    CT_EQ(hp_actsm_reported(&sm), 0);
    CT_EQ(map.current, 0);
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 0);
    free(cfg);
}

static void test_device_command(void) {
    setup();
    /* toggle device (amp): the direction is honoured against the tracked state */
    CT_EQ(hp_actsm_device_command(&sm, "amp", "power_off"), 0); /* tracked off: no IR */
    CT_EQ(nrec, 0);
    CT_EQ(hp_actsm_device_command(&sm, "amp", "power_on"), 0);
    EXPECT(NEC(9, 1));
    CT_CHECK(!strcmp(pw(1), "on"));
    CT_EQ(hp_actsm_device_command(&sm, "amp", "power_on"), 0); /* tracked on: no IR */
    CT_EQ(nrec, 1);
    CT_CHECK(!strcmp(pw(1), "on"));
    advance(g_now + 500);
    CT_EQ(hp_actsm_device_command(&sm, "amp", "input_opt"), 0); /* discrete: always sent */
    advance(g_now + 500);
    CT_EQ(hp_actsm_device_command(&sm, "amp", "input_opt"), 0);
    advance(g_now + 500);
    EXPECT(NEC(9, 1), NEC(9, 5), NEC(9, 5));
    CT_CHECK(!strcmp(in(1), "opt"));
    CT_EQ(hp_actsm_device_command(&sm, "amp", "power_off"), 0);
    advance(g_now + 500);
    EXPECT(NEC(9, 1), NEC(9, 5), NEC(9, 5), NEC(9, 1));
    CT_CHECK(!strcmp(pw(1), "off") && !strcmp(in(1), ""));
    /* on/off device (tv): discrete codes always sent; input implies power on */
    nrec = nev = 0;
    CT_EQ(hp_actsm_device_command(&sm, "tv", "power_on"), 0);
    advance(g_now + 500);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "power_on"), 0);
    advance(g_now + 500);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "input_hdmi2"), 0);
    advance(g_now + 500);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "vol_up"), 0); /* plain command: no state change */
    advance(g_now + 500);
    EXPECT(NEC(4, 1), NEC(4, 1), NEC(4, 11), NEC(4, 20));
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi2"));
    CT_EQ(hp_actsm_device_command(&sm, "tv", "power_off"), 0);
    advance(g_now + 500);
    CT_CHECK(!strcmp(pw(0), "off") && !strcmp(in(0), ""));
    CT_EQ(count_ev(HP_ACTSM_EVT_DEVICE), 3); /* tv: on, input, off (power_on twice = one change) */
}

static void test_device_command_errors_and_busy(void) {
    setup();
    CT_EQ(hp_actsm_device_command(&sm, "ghost", "power_on"), -HP_E_NOENT);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "nope"), -HP_E_INVAL);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "input_nope"), -HP_E_INVAL);
    CT_EQ(hp_actsm_device_command(&sm, "bd", "input_x"), -HP_E_INVAL);
    CT_EQ(nrec, 0);
    hp_actsm_start(&sm, "watch");
    CT_CHECK(hp_actsm_busy(&sm));
    size_t n = nrec;
    CT_EQ(hp_actsm_device_command(&sm, "tv", "vol_up"), 0); /* E28: queued, runs when the transition finishes */
    CT_EQ(nrec, n);
    settle_all();
    EXPECT(NEC(4, 1), NEC(9, 1), NEC(4, 10), NEC(9, 5), NEC(4, 20), NEC(4, 20));
    /* line owner: behind SIRC copies the send is deferred, not dropped or interleaved */
    nrec = 0;
    CT_EQ(hp_actsm_device_command(&sm, "bd", "power_on"), 0);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "vol_up"), 0);
    CT_EQ(nrec, 1);
    settle_all();
    EXPECT(SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 20));
    check_no_overlap();
}

/* ------------------------------------------------- audit batch: engine fixes */

/* D22: an input on a device tracked off powers it on first (settle respected); an input alone never marks power on. */
static void test_input_on_off_device_powers_on_first(void) {
    setup();
    CT_EQ(hp_actsm_device_command(&sm, "tv", "input_hdmi2"), 0);
    EXPECT(NEC(4, 1)); /* power-on only, now */
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), ""));
    advance(g_now + 200);
    CT_EQ(nrec, 1); /* still settling */
    advance(g_now + 400);
    EXPECT(NEC(4, 1), NEC(4, 11));
    CT_CHECK(recs[1].t - recs[0].t >= 300);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi2"));
    CT_EQ(hp_sched_pending(&sched), 0);
    /* a device with no power codes: the input is sent, the tracked power is not touched */
    setup();
    const char *c = "{\"format\":2,\"devices\":[{\"id\":\"x\",\"inputs\":{\"a\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2}}}],"
                    "\"activities\":[{\"id\":\"p\",\"devices\":[{\"device\":\"x\"}]}]}";
    setup_cfg(c);
    CT_EQ(hp_actsm_device_command(&sm, "x", "input_a"), 0);
    EXPECT(NEC(1, 2));
    CT_CHECK(!strcmp(pw(0), "off"));
}

/* D25: a deferred send's tracked-state change applies only once the frame is sent, and only on success. */
static int fail_sends;
static int flaky_send(void *self, uint8_t port, const hp_ir_signal_t *sig) {
    if (fail_sends) return -HP_E_IO;
    return rec_send(self, port, sig);
}
static hp_ir_sink flaky = {NULL, flaky_send};
static hp_sinks_t fsinks = {&flaky, NULL, NULL};

static void test_deferred_send_state_applies_on_success(void) {
    setup();
    sm.sinks = &fsinks;
    CT_EQ(hp_actsm_device_command(&sm, "bd", "power_on"), 0); /* SIRC copies keep the line busy */
    CT_CHECK(!strcmp(pw(0), "off"));
    CT_EQ(hp_actsm_device_command(&sm, "tv", "power_on"), 0); /* deferred behind the copies */
    CT_EQ(nrec, 1);
    CT_CHECK(!strcmp(pw(0), "off")); /* not on yet: nothing was sent */
    advance(g_now + 400);
    CT_CHECK(!strcmp(pw(0), "on"));
    EXPECT(SIRC(1, 21), SIRC(1, 21), SIRC(1, 21), NEC(4, 1));
    /* the deferred send fails: tracked state unchanged */
    setup();
    sm.sinks = &fsinks;
    fail_sends = 0;
    CT_EQ(hp_actsm_device_command(&sm, "bd", "power_on"), 0);
    CT_EQ(hp_actsm_device_command(&sm, "tv", "power_on"), 0);
    fail_sends = 1;
    advance(g_now + 400);
    fail_sends = 0;
    CT_CHECK(!strcmp(pw(0), "off"));
    CT_EQ(hp_sched_pending(&sched), 0);
}

/* D25: an aborted plan returns the error and tells every client (an ACTIVITY event for the reported activity). */
static void test_abort_returns_error_and_emits(void) {
    setup();
    sm.sinks = &fsinks;
    fail_sends = 1;
    nev = 0;
    CT_EQ(hp_actsm_start(&sm, "watch"), -HP_E_IO);
    fail_sends = 0;
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(map.current, -1);
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    CT_EQ(evs[nev - 1].idx, -1);
    /* a failure after the first step (timer-driven) also emits */
    setup();
    sm.sinks = &fsinks;
    hp_actsm_start(&sm, "watch");
    fail_sends = 1;
    nev = 0;
    advance(g_now + 1000);
    fail_sends = 0;
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    CT_EQ(hp_sched_pending(&sched), 0);
}

/* E26: BT keys always go out; IR keys queue (small FIFO), replay in order at finish, drop on cancel; a dropped key
 * is not forwarded to HA. */
static int emitted;
static void emit_cnt(void *c, uint8_t remote, uint8_t key, hp_key_action_t a) { (void)c; (void)remote; (void)key; (void)a; emitted++; }

static void test_keys_during_transition(void) {
    sinks.bt = &btsink;
    setup_cfg("{\"format\":2,\"devices\":[{\"id\":\"tv\",\"settle_ms\":300,\"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":1},"
              "\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2}},\"inputs\":{\"h\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":10}}}],"
              "\"activities\":[{\"id\":\"w\",\"devices\":[{\"device\":\"tv\",\"input\":\"h\"}]}],"
              "\"keys\":{\"6\":{\"type\":\"bt\",\"usage\":233,\"host\":3}}}");
    bt_n = bt_downs = 0;
    CT_EQ(hp_actsm_start(&sm, "w"), 0);
    CT_CHECK(hp_actsm_busy(&sm)); /* the input waits for the tv to settle */
    CT_EQ(hp_key_handle(&map, &sinks, 1, 6, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* BT key mid-transition */
    CT_EQ(bt_downs, 1);
    CT_EQ(hp_key_handle(&map, &sinks, 1, 6, HP_KEY_ACTION_RELEASE, NULL, NULL), 0);
    settle_all();
    sinks.bt = NULL;

    setup();
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    advance(g_now + 20);
    size_t n = nrec;
    for (int i = 0; i < 3; i++) CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, emit_cnt, NULL), 0);
    CT_EQ(nrec, n);
    CT_EQ(map.keyq_n, 3);
    settle_all();
    CT_EQ(map.keyq_n, 0);
    CT_EQ(nrec, 5 + 3); /* the plan, then all three presses */
    CT_EQ(recs[nrec - 1].h, NEC(5, 5));
    /* cancelled by a new start: the queue is dropped, not replayed */
    nrec = 0;
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(map.keyq_n, 1);
    CT_EQ(hp_actsm_start(&sm, "watch"), 0);
    CT_EQ(map.keyq_n, 0);
    settle_all();
    for (size_t i = 0; i < nrec; i++) CT_CHECK(recs[i].h != NEC(5, 5));
    /* a full queue drops the press without telling HA, and without its hold / release either */
    setup();
    hp_actsm_start(&sm, "watch");
    advance(g_now + 20);
    emitted = 0;
    for (int i = 0; i < HP_KEYQ_MAX; i++) hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, emit_cnt, NULL);
    CT_EQ(emitted, HP_KEYQ_MAX);
    hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, emit_cnt, NULL); /* the ninth: dropped */
    hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_HOLD, emit_cnt, NULL);
    hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_RELEASE, emit_cnt, NULL);
    CT_EQ(emitted, HP_KEYQ_MAX);
    settle_all();
}

/* E28: a queued ir_send runs at finish, in order after the queued device commands. */
static int irsend_n;
static char irsend_last[64];
static void irsend_cb(void *c, const char *json) { (void)c; irsend_n++; snprintf(irsend_last, sizeof irsend_last, "%s", json); }

static void test_queued_irsend(void) {
    setup();
    hp_actsm_set_irsend(&sm, irsend_cb, NULL);
    irsend_n = 0;
    hp_actsm_start(&sm, "watch");
    char *j = strdup("{\"x\":1}");
    CT_EQ(hp_actsm_defer_irsend(&sm, j), 0);
    CT_EQ(irsend_n, 0);
    settle_all();
    CT_EQ(irsend_n, 1);
    CT_CHECK(!strcmp(irsend_last, "{\"x\":1}"));
    /* the queue is bounded */
    hp_actsm_start(&sm, "movie");
    char *extra[HP_ACT_CMDQ_MAX + 1];
    int full = 0;
    for (int i = 0; i <= HP_ACT_CMDQ_MAX; i++) {
        extra[i] = strdup("{}");
        if (hp_actsm_defer_irsend(&sm, extra[i]) < 0) {
            full++;
            free(extra[i]);
        }
    }
    CT_EQ(full, 1);
    settle_all();
    CT_EQ(irsend_n, 1 + HP_ACT_CMDQ_MAX);
}

/* D23: a state file of another config restores what still exists, by id. */
static void test_state_restore_by_id_other_config(void) {
    setup();
    tmp_path();
    hp_actsm_set_state_path(&sm, path);
    hp_actsm_start(&sm, "watch");
    settle_all();
    CT_EQ(hp_actsm_flush_state(&sm), 0);
    const char *nw = "{\"format\":2,\"devices\":[{\"id\":\"tv\",\"inputs\":{\"hdmi1\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":10}}},{\"id\":\"zz\"}],"
                     "\"activities\":[{\"id\":\"other\",\"devices\":[{\"device\":\"zz\"}]}]}";
    setup_cfg(nw);
    hp_actsm_set_state_path(&sm, path);
    CT_EQ(hp_actsm_load_state(&sm), 0);
    CT_CHECK(!strcmp(pw(0), "on") && !strcmp(in(0), "hdmi1")); /* tv survived */
    CT_CHECK(!strcmp(pw(1), "off"));
    CT_EQ(hp_actsm_reported(&sm), -1); /* "watch" is gone */
    unlink(path);
}

/* The off_press marker rides only on the report that finishes an actual Off, also while already Off. */
static void test_off_press_marker(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    CT_EQ(count_off_ev(), 0); /* a start is not an Off */
    hp_actsm_start(&sm, "watch"); /* re-pressed: resync */
    settle_all();
    CT_EQ(count_off_ev(), 0);
    CT_EQ(hp_map_load(&map, CFG, strlen(CFG)), 0);
    hp_actsm_reloaded(&sm); /* reload */
    settle_all();
    CT_EQ(count_off_ev(), 0);
    CT_EQ(hp_actsm_off(&sm), 0);
    settle_all();
    CT_EQ(count_off_ev(), 1);
    nev = 0;
    CT_EQ(hp_actsm_off(&sm), 0); /* already Off: the press still reports */
    settle_all();
    CT_EQ(count_ev(HP_ACTSM_EVT_ACTIVITY), 1);
    CT_EQ(count_off_ev(), 1);
    nev = 0;
    CT_EQ(hp_map_load(&map, CFG, strlen(CFG)), 0);
    hp_actsm_reloaded(&sm); /* reload while Off: no marker */
    settle_all();
    CT_EQ(count_off_ev(), 0);
    CT_CHECK(!hp_actsm_off_press(&sm)); /* only visible inside the notify */
}

/* ------------------------------------------------- review follow-ups */

/* A same-hash reload mid-Off keeps the remaining off steps (the plan is not rebuilt from the shrunk tracked state). */
static void test_same_hash_reload_keeps_off_plan(void) {
    setup();
    hp_actsm_start(&sm, "watch");
    settle_all();
    nrec = 0;
    CT_EQ(hp_actsm_off(&sm), 0);
    advance(g_now + 20); /* tv off went out; amp toggle and watch's off steps pending */
    CT_EQ(nrec, 1);
    CT_EQ(hp_map_load(&map, CFG, strlen(CFG)), 0);
    hp_actsm_reloaded(&sm);
    CT_CHECK(hp_actsm_busy(&sm));
    settle_all();
    EXPECT(NEC(4, 2), NEC(9, 1), NEC(7, 1));
    CT_EQ(hp_actsm_reported(&sm), -1);
}

/* An input queued behind a power-on is dropped when an Off powers the device down before it is sent. */
static void test_queued_input_dropped_by_off(void) {
    setup();
    CT_EQ(hp_actsm_device_command(&sm, "tv", "input_hdmi2"), 0);
    EXPECT(NEC(4, 1));
    CT_EQ(hp_actsm_off(&sm), 0);
    settle_all();
    for (size_t i = 0; i < nrec; i++) CT_CHECK(recs[i].h != NEC(4, 11));
    CT_CHECK(!strcmp(pw(0), "off"));
    CT_EQ(sm.cq_n, 0);
}

/* A reload that removes the running target still runs the queued commands (and frees them). */
static void test_reload_removing_target_drains_queue(void) {
    setup();
    hp_actsm_set_irsend(&sm, irsend_cb, NULL);
    irsend_n = 0;
    hp_actsm_start(&sm, "watch");
    CT_EQ(hp_actsm_defer_irsend(&sm, strdup("{\"y\":2}")), 0);
    const char *nw = "{\"format\":2,\"devices\":[{\"id\":\"tv\"}],\"activities\":[{\"id\":\"movie\",\"devices\":[{\"device\":\"tv\"}]}]}";
    CT_EQ(hp_map_load(&map, nw, strlen(nw)), 0);
    hp_actsm_reloaded(&sm);
    CT_CHECK(!hp_actsm_busy(&sm));
    CT_EQ(irsend_n, 1);
    CT_EQ(sm.cq_n, 0);
}

/* Queued keys replay once the devices have settled, not at the last frame. */
static void test_key_replay_waits_for_settle(void) {
    setup();
    CT_EQ(hp_actsm_start(&sm, "movie"), 1);
    advance(g_now + 20);
    CT_EQ(hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(map.keyq_n, 1);
    settle_all();
    size_t in = 0, key = 0;
    for (size_t i = 0; i < nrec; i++) {
        if (recs[i].h == NEC(4, 11)) in = i;
        if (recs[i].h == NEC(5, 5)) key = i;
    }
    CT_CHECK(key > in);
    CT_CHECK(recs[key].t - recs[in].t >= 300);
    CT_EQ(hp_sched_pending(&sched), 0);
    /* a new transition cancels the pending replay */
    setup();
    hp_actsm_start(&sm, "movie");
    advance(g_now + 20);
    hp_key_handle(&map, &sinks, 1, 4, HP_KEY_ACTION_PRESS, NULL, NULL);
    advance(g_now + 700); /* movie finished, replay waiting on tv's settle */
    CT_CHECK(!hp_actsm_busy(&sm));
    size_t n = nrec;
    CT_EQ(hp_actsm_start(&sm, "music"), 2);
    settle_all();
    CT_CHECK(nrec > n);
    for (size_t i = n; i < nrec; i++) CT_CHECK(recs[i].h != NEC(5, 5));
}

int main(void) {
    CT_RUN(test_device_command);
    CT_RUN(test_device_command_errors_and_busy);
    CT_RUN(test_start_from_off);
    CT_RUN(test_a_to_b_shared_devices);
    CT_RUN(test_toggle_never_sent_when_in_wanted_state);
    CT_RUN(test_unknown_input_is_sent);
    CT_RUN(test_repress_resyncs_stale_discrete_state);
    CT_RUN(test_switch_still_skips_tracked);
    CT_RUN(test_off);
    CT_RUN(test_off_with_unknown_state);
    CT_RUN(test_input_on_off_device_powers_on_first);
    CT_RUN(test_deferred_send_state_applies_on_success);
    CT_RUN(test_abort_returns_error_and_emits);
    CT_RUN(test_keys_during_transition);
    CT_RUN(test_queued_irsend);
    CT_RUN(test_same_hash_reload_keeps_off_plan);
    CT_RUN(test_queued_input_dropped_by_off);
    CT_RUN(test_reload_removing_target_drains_queue);
    CT_RUN(test_key_replay_waits_for_settle);
    CT_RUN(test_state_restore_by_id_other_config);
    CT_RUN(test_interrupted_transition);
    CT_RUN(test_interrupt_keeps_settle);
    CT_RUN(test_off_during_transition);
    CT_RUN(test_set_device_during_transition_skips_redundant);
    CT_RUN(test_drops_are_logged);
    CT_RUN(test_set_device_errors);
    CT_RUN(test_emitted_key_layer_is_the_lookup_layer);
    CT_RUN(test_key_driven_and_busy_keys);
    CT_RUN(test_bt_key_released_on_switch_and_off);
    CT_RUN(test_start_event_and_activity_bt_hosts);
    CT_RUN(test_format1_unchanged);
    CT_RUN(test_reload_keeps_state_by_id);
    CT_RUN(test_state_roundtrip);
    CT_RUN(test_state_off_roundtrip);
    CT_RUN(test_state_bad_files);
    CT_RUN(test_state_debounce);
    CT_RUN(test_idle_clock_wrap);
    CT_RUN(test_off_press_marker);
    CT_RUN(test_reload_mid_transition_restarts);
    CT_RUN(test_reload_rename_keeps_current);
    CT_RUN(test_plan_waits_for_sirc_copies);
    CT_RUN(test_press_after_finish_deferred);
    CT_RUN(test_hold_repeat_vs_plan_start);
    hp_map_free(&map);
    return CT_DONE();
}
