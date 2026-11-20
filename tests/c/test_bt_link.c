/* The Bluetooth supervisor (hub/bt_link.c) against tests/c/bt_fake_helper (a test-only program speaking bt_ipc, no Bluetooth).
 * Real child processes and sockets; the supervisor's clock is virtual (g_now), so 15 s of silence or a 120 s backoff cost nothing.
 * argv[1] = path of the fake helper. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/bt_link.h"
#include "hub/bt_link_sup.h"
#include "hub/hub_handlers.h"
#include "hub/io/sim_sinks.h"

static const char *g_helper;
static char g_dir[128], g_log[160];
static uint64_t g_now;
static uint64_t vnow(void) { return g_now; }

/* the stock bluetoothd stand-in */
static int bz_running, bz_stubborn, bz_calls, bz_term, bz_kill;
static int bz_sig(int sig) {
    bz_calls++;
    if (sig == SIGTERM) {
        bz_term++;
        if (!bz_stubborn) bz_running = 0;
        return 1;
    }
    if (sig == SIGKILL) {
        bz_kill++;
        bz_running = 0;
        return 1;
    }
    return bz_running;
}

/* what the engine and the listener saw */
static int ev_listener, ev_release, ev_lost_n, ev_key_n, ev_rcb_n;
static unsigned ev_lost[64];
static struct { unsigned slot, page, usage; bool down; } ev_key[40000], ev_rcb[64];
static void on_listener(void *c) { (void)c; ev_listener++; }
static void on_eng_key(void *c, unsigned slot, unsigned page, unsigned usage, bool down) {
    (void)c;
    if (ev_key_n < 40000) { ev_key[ev_key_n].slot = slot; ev_key[ev_key_n].page = page; ev_key[ev_key_n].usage = usage; ev_key[ev_key_n].down = down; }
    ev_key_n++;
}
static void on_eng_lost(void *c, unsigned slot) {
    (void)c;
    if (ev_lost_n < 64) ev_lost[ev_lost_n] = slot;
    ev_lost_n++;
}
static void on_eng_release(void *c) { (void)c; ev_release++; }
static void on_rcb(void *c, unsigned slot, unsigned page, unsigned usage, bool down) {
    (void)c;
    if (ev_rcb_n < 64) { ev_rcb[ev_rcb_n].slot = slot; ev_rcb[ev_rcb_n].page = page; ev_rcb[ev_rcb_n].usage = usage; ev_rcb[ev_rcb_n].down = down; }
    ev_rcb_n++;
}
static const struct bt_link_engine ENG = {NULL, on_eng_key, on_eng_lost, on_eng_release};

static long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void setup(const char *mode) {
    bt_link_reset();
    g_now = 1000000;
    bz_running = bz_stubborn = bz_calls = bz_term = bz_kill = 0;
    ev_listener = ev_release = ev_lost_n = ev_key_n = ev_rcb_n = 0;
    snprintf(g_log, sizeof g_log, "%s/log", g_dir);
    unlink(g_log);
    setenv("BT_FAKE_MODE", mode, 1);
    setenv("BT_FAKE_LOG", g_log, 1);
    struct bt_link_hooks hk = {0};
    hk.now_ms = vnow;
    hk.bluetoothd_signal = bz_sig;
    hk.helper_exe = g_helper;
    hk.data_dir = g_dir;
    bt_link_configure(&hk);
    bt_link_set_engine(&ENG);
    bt_link_set_listener(on_listener, NULL);
    bt_link_set_remote_key_cb(on_rcb, NULL);
}

static void pump_once(void) {
    int fd = bt_link_poll_fd();
    if (fd >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        poll(&p, 1, 5);
    } else {
        usleep(3000);
    }
    bt_link_service();
}
#define PUMP(expr, ms) ({ int ok_ = 0; long t_ = mono_ms() + (ms); while (!(ok_ = !!(expr)) && mono_ms() < t_) pump_once(); ok_; })
static void pump_for(long ms) {
    long t = mono_ms() + ms;
    while (mono_ms() < t) pump_once();
}
static void advance(uint64_t ms) {
    g_now += ms;
    bt_link_service();
}

/* Virtual time passes in steps the helper can answer a PING in (a jump over 15 s would look like silence). */
static void advance_alive(uint64_t ms) {
    while (ms) {
        uint64_t step = ms > 4000 ? 4000 : ms;
        g_now += step;
        ms -= step;
        bt_link_service();
        pump_for(8);
    }
}

static int log_count(const char *needle) {
    FILE *f = fopen(g_log, "r");
    if (!f) return 0;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static struct bt_link_state S;
static const struct bt_link_state *st(void) {
    bt_link_state(&S);
    return &S;
}
static struct bt_link_stats stats(void) {
    struct bt_link_stats s;
    bt_link_stats(&s);
    return s;
}
static void cfg(unsigned nh, const char *const *haddr, const unsigned *hslot, unsigned nr, const char *const *raddr,
                const unsigned *rslot) {
    struct bt_link_cfg_entry h[8], r[8];
    for (unsigned i = 0; i < nh; i++) { h[i].slot = (uint8_t)hslot[i]; snprintf(h[i].addr, sizeof h[i].addr, "%s", haddr[i]); }
    for (unsigned i = 0; i < nr; i++) { r[i].slot = (uint8_t)rslot[i]; snprintf(r[i].addr, sizeof r[i].addr, "%s", raddr[i]); }
    bt_link_set_config(h, nh, r, nr);
}
/* The usual: hosts slot 3 = the bonded Shield, slot 0 = an address that is not bonded; remote slot 2 = the bonded remote. */
static void usual_cfg(void) {
    const char *ha[] = {"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:99"};
    const unsigned hs[] = {3, 0};
    const char *ra[] = {"11:22:33:44:55:01"};
    const unsigned rs[] = {2};
    cfg(2, ha, hs, 1, ra, rs);
}
static void no_cfg(void) { bt_link_set_config(NULL, 0, NULL, 0); }
static bool ready(void) { return st()->status == BT_READY && st()->n_hosts > 0 && st()->n_remotes > 0; }

static void test_spawn_binding_and_stop(void) {
    setup("normal");
    bt_link_service();
    CT_EQ(stats().pid, 0); /* no config, no window: no helper */
    CT_EQ(stats().spawns, 0);
    usual_cfg();
    bt_link_service();
    CT_CHECK(stats().pid > 0);
    CT_EQ(st()->status, BT_STARTING);
    CT_CHECK(PUMP(ready(), 3000));
    struct bt_link_limits lim;
    bt_link_limits(&lim);
    CT_EQ(lim.hosts_max, 255);
    CT_EQ(lim.conn_max, 4);
    CT_EQ(lim.remotes_max, 32);
    CT_CHECK(lim.le);
    CT_CHECK(st()->gen > 0 && ev_listener > 0);
    /* hosts: config slot 3 is the bonded Shield; the other bonded host (Old TV) is not in the config and gets a free
     * number that no config entry uses (config slots are 0 and 3, so 1) */
    CT_EQ(st()->n_hosts, 2);
    CT_EQ(st()->hosts[0].slot, 1);
    CT_CHECK(!st()->hosts[0].configured && !strcmp(st()->hosts[0].name, "Old TV") && st()->hosts[0].transport == BT_TRANSPORT_CLASSIC);
    CT_EQ(st()->hosts[1].slot, 3);
    CT_CHECK(st()->hosts[1].configured && !strcmp(st()->hosts[1].addr, "AA:BB:CC:DD:EE:01") && st()->hosts[1].transport == BT_TRANSPORT_LE);
    CT_EQ(st()->n_remotes, 2);
    CT_EQ(st()->remotes[0].slot, 0);
    CT_CHECK(!st()->remotes[0].configured && st()->remotes[0].battery == -1);
    CT_EQ(st()->remotes[1].slot, 2);
    CT_CHECK(st()->remotes[1].configured && st()->remotes[1].connected && st()->remotes[1].battery == 80);
    /* keys go to the helper's own slot of the bonded address */
    CT_EQ(bt_link_key(3, BT_PAGE_KEYBOARD, 0x28, BT_KEY_PRESS), BT_LINK_OK);
    CT_CHECK(PUMP(log_count("KEY 0 7 40 1") == 1 && log_count("KEY 0 7 40 0") == 1, 2000));
    CT_EQ(bt_link_key(0, BT_PAGE_KEYBOARD, 0x28, BT_KEY_PRESS), BT_LINK_E_NO_SLOT); /* configured host, address not bonded */
    CT_EQ(bt_link_key(200, BT_PAGE_KEYBOARD, 0x28, BT_KEY_DOWN), BT_LINK_E_NO_SLOT);
    CT_EQ(bt_link_key(1, BT_PAGE_CONSUMER, 0xE9, BT_KEY_DOWN), BT_LINK_OK); /* the unconfigured one, by its reported slot */
    CT_CHECK(PUMP(log_count("KEY 5 12 233 1") == 1, 2000));
    bt_link_connect(3);
    CT_EQ(bt_link_forget(BT_KIND_HOST, 1), BT_LINK_OK);
    CT_EQ(bt_link_forget(BT_KIND_REMOTE, 0), BT_LINK_OK);
    CT_EQ(bt_link_forget(BT_KIND_HOST, 0), BT_LINK_E_NO_SLOT);
    CT_CHECK(PUMP(log_count("CONNECT 0") == 1 && log_count("FORGET 0 5") == 1 && log_count("FORGET 1 1") == 1, 2000));
    /* a new config without Bluetooth stops the helper: not a failure */
    int pid = stats().pid;
    no_cfg();
    advance(0);
    CT_EQ(stats().pid, 0);
    CT_EQ(st()->status, BT_OFF);
    CT_EQ(st()->n_hosts, 0);
    CT_EQ(stats().deaths, 0);
    CT_CHECK(PUMP(kill(pid, 0) != 0, 3000)); /* SIGTERM, reaped */
    bt_link_reset();
}

static void test_death_clears_state(void) {
    setup("normal");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    CT_CHECK(PUMP(ev_lost_n == 0, 10)); /* nothing lost yet */
    unsigned gen = st()->gen;
    int l0 = ev_listener, r0 = ev_release;
    kill(stats().pid, SIGKILL);
    CT_CHECK(PUMP(st()->status != BT_READY, 3000));
    CT_EQ(st()->status, BT_OFF);
    CT_EQ(st()->n_hosts, 0);
    CT_EQ(st()->n_remotes, 0);
    CT_CHECK(st()->gen > gen);
    CT_CHECK(ev_listener > l0);
    CT_CHECK(ev_release > r0);               /* a held key is let go */
    CT_EQ(ev_lost_n, 1);                     /* the connected configured remote is reported lost, once... */
    CT_EQ(ev_lost[0], 2);                    /* ... by its config slot */
    CT_EQ(stats().deaths, 1);
    CT_EQ(bt_link_key(3, 7, 0x28, BT_KEY_DOWN), BT_LINK_E_NOT_READY);
    advance(4999);
    CT_EQ(stats().pid, 0); /* the backoff */
    advance(2);
    CT_CHECK(PUMP(stats().pid > 0, 3000)); /* once the old one is reaped */
    CT_CHECK(PUMP(ready(), 3000));
    bt_link_reset();
}

static void test_remote_keys(void) {
    setup("rkey");
    usual_cfg();
    CT_CHECK(PUMP(ev_rcb_n >= 4, 4000));
    CT_EQ(ev_key_n, 2); /* the configured remote's keys reach the engine, by config slot */
    CT_CHECK(ev_key[0].slot == 2 && ev_key[0].page == 12 && ev_key[0].usage == 0xE9 && ev_key[0].down);
    CT_CHECK(ev_key[1].slot == 2 && !ev_key[1].down);
    /* every key reaches the bt_listen callback, the unconfigured remote under its reported slot (0) */
    CT_CHECK(ev_rcb[0].slot == 2 && ev_rcb[1].slot == 0 && ev_rcb[1].page == 7 && ev_rcb[1].usage == 0x28);
    bt_link_reset();
}

static void test_remote_disconnect(void) {
    setup("remote_drop");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    CT_EQ(ev_lost_n, 0);
    CT_CHECK(PUMP(ev_lost_n >= 1, 3000));
    pump_for(50);
    CT_EQ(ev_lost_n, 1);
    CT_EQ(ev_lost[0], 2);
    CT_CHECK(!st()->remotes[1].connected);
    CT_EQ(stats().deaths, 0);
    bt_link_reset();
}

static void test_backoff_and_cap(void) {
    setup("crash_start");
    usual_cfg();
    bt_link_service();
    static const unsigned delay_s[5] = {5, 30, 120, 120, 120};
    for (unsigned k = 0; k < 5; k++) {
        CT_CHECK(PUMP(stats().deaths == k + 1, 3000));
        pump_for(20);
        CT_EQ(stats().spawns, k + 1);
        CT_EQ(st()->status, BT_OFF);
        advance((uint64_t)delay_s[k] * 1000 - 1);
        pump_for(10);
        CT_EQ(stats().spawns, k + 1); /* not yet */
        advance(2);
        CT_CHECK(PUMP(stats().spawns == k + 2, 3000));
    }
    CT_CHECK(PUMP(stats().deaths == 6, 3000));
    pump_for(20);
    CT_EQ(st()->status, BT_DISABLED);
    advance(10 * 1000 * 1000);
    pump_for(10);
    CT_EQ(stats().spawns, 6); /* at most 5 respawns a boot */
    CT_EQ(bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_HOST), BT_LINK_E_NOT_READY);
    CT_EQ(st()->status, BT_DISABLED);
    bt_link_reset();
}

static void test_no_adapter(void) {
    setup("noadapter");
    usual_cfg();
    CT_CHECK(PUMP(stats().deaths == 1, 3000));
    CT_EQ(st()->status, BT_NO_ADAPTER);
    CT_EQ(bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_HOST), BT_LINK_E_NOT_READY);
    bt_link_reset();
}

static void test_silence_kills(void) {
    setup("hang");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    advance(5000);
    advance(5000);
    advance(4999);
    CT_CHECK(stats().pid > 0); /* 14.999 s of silence */
    pump_for(20);
    CT_EQ(stats().deaths, 0);
    advance(1);
    CT_EQ(stats().deaths, 1);
    CT_EQ(stats().pid, 0);
    CT_EQ(st()->status, BT_OFF);
    CT_EQ(stats().bad_frames, 0);
    bt_link_reset();
}

static void test_ping_keeps_alive(void) {
    setup("normal");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    for (int i = 1; i <= 6; i++) { /* 30 s of virtual time, a PING every 5 s answered by a PONG */
        g_now += 5000;
        bt_link_service();
        CT_CHECK(PUMP(log_count("PING") >= i, 2000));
        pump_for(30);
    }
    CT_EQ(stats().deaths, 0);
    CT_EQ(st()->status, BT_READY);
    CT_EQ(log_count("PING"), 6);
    bt_link_reset();
}

static void test_malformed_frames(void) {
    static const char *const modes[] = {"garbage_len", "garbage_type", "garbage_hosts"};
    for (unsigned i = 0; i < 3; i++) {
        setup(modes[i]);
        usual_cfg();
        CT_CHECK(PUMP(stats().deaths == 1, 3000));
        CT_EQ(stats().bad_frames, 1);
        CT_EQ(st()->status, BT_OFF);
        CT_EQ(st()->n_hosts, 0);
        bt_link_reset();
    }
}

static void test_partial_frames(void) {
    setup("dribble"); /* the helper's first frames arrive one byte at a time */
    usual_cfg();
    CT_CHECK(PUMP(ready() && st()->n_remotes == 2, 5000));
    CT_EQ(stats().deaths, 0);
    bt_link_reset();
}

static void test_flood(void) {
    setup("flood");
    usual_cfg();
    long worst = 0, t0 = mono_ms();
    int per_pass_max = 0, wait = 5;
    while (ev_key_n < 15000 && mono_ms() - t0 < 20000) {
        int fd = bt_link_poll_fd();
        if (fd >= 0 && wait != 0) { struct pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 5); }
        long a = mono_ms();
        int before = ev_rcb_n;
        wait = bt_link_service(); /* 0 = frames still buffered: no waiting */
        if (ev_rcb_n - before > per_pass_max) per_pass_max = ev_rcb_n - before;
        if (mono_ms() - a > worst) worst = mono_ms() - a;
    }
    CT_EQ(ev_key_n, 15000); /* every key of the configured remote arrived, none of the other */
    CT_CHECK(worst < 150);  /* no pass is long: the loop stays responsive under a flood */
    CT_CHECK(per_pass_max > 0 && per_pass_max <= 32); /* at most 32 remote keys (so key_event broadcasts) per pass */
    CT_EQ(stats().deaths, 0);
    CT_EQ(st()->n_hosts, 2);
    bt_link_reset();
}

static void test_slow_reader(void) {
    setup("slow");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    long worst = 0, t0 = mono_ms();
    CT_EQ(bt_link_key(3, BT_PAGE_KEYBOARD, 0x05, BT_KEY_DOWN), BT_LINK_OK); /* held before the socket fills */
    for (int i = 0; i < 100000; i++) {
        long a = mono_ms();
        int rc = bt_link_key(3, BT_PAGE_KEYBOARD, 0x28, BT_KEY_PRESS);
        CT_CHECK(rc == BT_LINK_OK || rc == BT_LINK_E_BUSY);
        if (mono_ms() - a > worst) worst = mono_ms() - a;
    }
    CT_CHECK(mono_ms() - t0 < 5000);
    CT_CHECK(worst < 50);                  /* never waits for the helper */
    CT_CHECK(stats().key_drops > 0);       /* the socket filled: keys dropped and counted */
    /* a DOWN is dropped when full, its UP then too (no orphan); the UP of a key that did go down is never dropped */
    unsigned d0 = stats().key_drops;
    CT_EQ(bt_link_key(3, BT_PAGE_KEYBOARD, 0x06, BT_KEY_DOWN), BT_LINK_E_BUSY);
    CT_EQ(stats().key_drops, d0 + 1);
    CT_EQ(bt_link_key(3, BT_PAGE_KEYBOARD, 0x06, BT_KEY_UP), BT_LINK_OK); /* swallowed with its DOWN */
    CT_EQ(bt_link_key(3, BT_PAGE_KEYBOARD, 0x05, BT_KEY_UP), BT_LINK_OK); /* queued in the reserve, not dropped */
    CT_EQ(stats().key_drops, d0 + 1);
    int busy = 0;
    for (int i = 0; i < 400; i++)
        if (bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_HOST) == BT_LINK_E_BUSY) busy++;
    CT_CHECK(busy > 0); /* the control queue is bounded */
    long a = mono_ms();
    bt_link_service();
    CT_CHECK(mono_ms() - a < 50);
    bt_link_reset();
}

/* a kernel without /proc/<pid>/comm (the hub's 2.6.31): the name comes from stat or cmdline */
static void put_n(const char *dir, long pid, const char *file, const char *text, size_t len) {
    char d[160], p[200];
    snprintf(d, sizeof d, "%s/%ld", dir, pid);
    mkdir(d, 0700);
    snprintf(p, sizeof p, "%s/%s", d, file);
    FILE *f = fopen(p, "w");
    fwrite(text, 1, len, f); /* explicit length: cmdline holds NULs */
    fclose(f);
}
#define put(d, pid, file, text) put_n(d, pid, file, text, sizeof(text) - 1)

static void test_bluetoothd_find(void) {
    char root[] = "/tmp/hh_proc_XXXXXX";
    CT_CHECK(mkdtemp(root) != NULL);
    put(root, 100, "stat", "100 (hciattach) S 1 100 100 0");           /* must not match */
    put(root, 200, "stat", "200 (bluetoothd) S 1 200 200 0");          /* stat only */
    put(root, 300, "cmdline", "/usr/sbin/bluetoothd\0-n\0");          /* cmdline only */
    put(root, 400, "comm", "bluetoothd\n");                            /* newer kernels */
    put(root, 500, "stat", "500 (blue (toothd)) S 1 500 500 0");       /* odd name, not it */
    put(root, 600, "stat", "600 (bluetoothd-x) S 1 600 600 0");        /* a prefix is not it */
    put(root, 700, "stat", "700 (bluetoothd) S 1 700 700 0");          /* skipped as self */
    put(root, 800, "stat", "800 (bluetoothd) Z 1 800 800 0");          /* zombie: not running */
    put(root, 900, "stat", "900 (bluetoothd) X 1 900 900 0");          /* dead */
    long pids[16];
    CT_EQ(bt_link_find_bluetoothd(root, 700, pids, 16), 3);
    CT_CHECK(pids[0] + pids[1] + pids[2] == 200 + 300 + 400);
    CT_EQ(bt_link_find_bluetoothd("/nonexistent_hh_proc", 1, pids, 16), 0);
    static const long ids[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};
    static const char *const fs[] = {"stat", "stat", "cmdline", "comm", "stat", "stat", "stat", "stat", "stat"};
    for (int i = 0; i < 9; i++) {
        char p[200];
        snprintf(p, sizeof p, "%s/%ld/%s", root, ids[i], fs[i]);
        unlink(p);
        snprintf(p, sizeof p, "%s/%ld", root, ids[i]);
        rmdir(p);
    }
    rmdir(root);
}

static void test_bluetoothd(void) {
    setup("normal"); /* running: stopped once, before the first spawn only */
    bz_running = 1;
    usual_cfg();
    CT_CHECK(PUMP(stats().spawns == 1, 2000));
    CT_EQ(bz_term, 1);
    CT_EQ(bz_kill, 0);
    CT_CHECK(PUMP(ready(), 3000));
    int calls = bz_calls;
    kill(stats().pid, SIGKILL);
    CT_CHECK(PUMP(stats().deaths == 1, 3000));
    advance(5001);
    CT_CHECK(PUMP(stats().spawns == 2, 2000));
    CT_EQ(bz_term, 1);
    CT_EQ(bz_calls, calls); /* not touched again this boot */
    bt_link_reset();

    setup("normal"); /* not running: never signalled */
    usual_cfg();
    CT_CHECK(PUMP(stats().spawns == 1, 2000));
    CT_EQ(bz_term, 0);
    CT_EQ(bz_kill, 0);
    CT_EQ(bz_calls, 1); /* one probe */
    bt_link_reset();

    setup("normal"); /* a daemon that ignores SIGTERM: SIGKILL after 3 s, then the spawn goes ahead anyway */
    bz_running = bz_stubborn = 1;
    usual_cfg();
    bt_link_service();
    CT_EQ(stats().spawns, 0);
    advance(3001);
    CT_EQ(bz_kill, 1);
    CT_EQ(stats().spawns, 0);
    advance(1001);
    CT_EQ(stats().spawns, 1);
    bt_link_reset();

    setup("normal"); /* --sim: never */
    struct bt_link_hooks hk = {0};
    hk.now_ms = vnow;
    hk.bluetoothd_signal = bz_sig;
    hk.helper_exe = g_helper;
    hk.data_dir = g_dir;
    hk.skip_bluetoothd = 1;
    bt_link_configure(&hk);
    bz_running = 1;
    usual_cfg();
    CT_CHECK(PUMP(stats().spawns == 1, 2000));
    CT_EQ(bz_calls, 0);
    bt_link_reset();
}

static void test_pairing_windows(void) {
    setup("normal"); /* a pair request starts the helper even with no config */
    CT_EQ(st()->window, BT_WIN_NONE);
    CT_EQ(bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_REMOTE), BT_LINK_OK);
    CT_EQ(st()->status, BT_STARTING);
    CT_EQ(st()->window, BT_WIN_REMOTE); /* "window open, nothing seen yet" */
    CT_EQ(st()->n_candidates, 0);
    CT_CHECK(PUMP(st()->n_candidates == 2, 4000));
    CT_EQ(log_count("PAIR 1 60 1"), 1);
    CT_EQ(st()->window, BT_WIN_REMOTE);
    CT_CHECK(!strcmp(st()->candidates[0].addr, "11:22:33:44:55:77") && !strcmp(st()->candidates[0].name, "Click") &&
             st()->candidates[0].rssi == -50);
    CT_CHECK(st()->candidates[1].name[0] == 0 && st()->candidates[1].rssi == -70);
    CT_EQ(bt_link_pair_remote("11:22:33:44:55:AA"), BT_LINK_E_NO_SLOT); /* not a candidate */
    CT_EQ(bt_link_pair_remote("11:22:33:44:55:77"), BT_LINK_OK);
    CT_CHECK(PUMP(st()->pairing == BT_PAIR_OK && st()->window == BT_WIN_NONE, 3000));
    CT_EQ(log_count("PAIR_REMOTE 11:22:33:44:55:77 1"), 1);
    CT_EQ(st()->pairing_kind, BT_KIND_REMOTE);
    CT_CHECK(!strcmp(st()->pairing_addr, "11:22:33:44:55:09") && !strcmp(st()->pairing_name, "Picked") &&
             st()->pairing_transport == BT_TRANSPORT_LE);
    CT_EQ(st()->n_candidates, 0);
    CT_EQ(st()->n_remotes, 3); /* bonded but not configured: still reported, so the editor can offer it */
    CT_CHECK(!st()->remotes[2].configured && !strcmp(st()->remotes[2].name, "Picked"));
    CT_CHECK(stats().pid > 0); /* kept for a while so the result can be read ... */
    advance_alive(BT_SUP_LINGER_MS - 1);
    CT_CHECK(stats().pid > 0);
    advance_alive(2);
    CT_EQ(stats().pid, 0); /* ... then stopped: no config, no window */
    CT_EQ(stats().deaths, 0);
    bt_link_reset();

    setup("normal"); /* opening a new window clears the previous outcome (gen bump + listener): a stale "ok" is never the new result */
    CT_EQ(bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_REMOTE), BT_LINK_OK);
    CT_CHECK(PUMP(st()->n_candidates == 2, 4000));
    CT_EQ(bt_link_pair_remote("11:22:33:44:55:77"), BT_LINK_OK);
    CT_CHECK(PUMP(st()->pairing == BT_PAIR_OK && st()->window == BT_WIN_NONE, 3000));
    {
        uint32_t g0 = st()->gen;
        int l0 = ev_listener;
        CT_EQ(bt_link_pair(true, 60, BT_TRANSPORT_ANY, BT_MODE_HOST), BT_LINK_OK);
        CT_EQ(st()->pairing, BT_PAIR_NONE);
        CT_CHECK(st()->pairing_addr[0] == 0 && st()->pairing_name[0] == 0 && st()->pairing_transport == BT_TRANSPORT_ANY);
        bt_link_service(); /* the push is made by the service pass */
        CT_CHECK(st()->gen != g0 && ev_listener > l0);
    }
    bt_link_reset();

    setup("normal"); /* a host window counts down by itself and can be closed */
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    CT_EQ(bt_link_pair(true, 120, BT_TRANSPORT_CLASSIC, BT_MODE_HOST), BT_LINK_OK);
    CT_CHECK(st()->discoverable && st()->window == BT_WIN_HOST && st()->discoverable_secs == 120);
    CT_CHECK(PUMP(log_count("PAIR 1 120 0") == 1, 2000));
    unsigned gen = st()->gen;
    g_now += 20000;
    CT_EQ(st()->discoverable_secs, 100);
    CT_EQ(st()->gen, gen); /* the countdown never bumps gen */
    CT_EQ(bt_link_pair(false, 0, BT_TRANSPORT_ANY, BT_MODE_HOST), BT_LINK_OK);
    CT_EQ(st()->window, BT_WIN_NONE);
    CT_CHECK(PUMP(log_count("PAIR 0") == 1, 2000));
    CT_CHECK(PUMP(log_count("PAIRTR 1") == 1, 2000)); /* the classic window carried its transport to the helper */
    CT_EQ(bt_link_pair(true, 30, BT_TRANSPORT_LE, BT_MODE_HOST), BT_LINK_OK);
    CT_CHECK(PUMP(log_count("PAIRTR 2") == 1, 2000));
    CT_EQ(bt_link_pair(true, 30, BT_TRANSPORT_ANY, BT_MODE_HOST), BT_LINK_OK);
    CT_CHECK(PUMP(log_count("PAIRTR 0") == 1, 2000));
    bt_link_reset();
}

static void test_stubborn_stop(void) {
    setup("stubborn");
    usual_cfg();
    CT_CHECK(PUMP(ready(), 3000));
    int pid = stats().pid;
    no_cfg();
    advance(0); /* SIGTERM: ignored */
    CT_EQ(stats().pid, 0);
    pump_for(100);
    CT_EQ(kill(pid, 0), 0);
    advance(2500); /* SIGKILL */
    CT_CHECK(PUMP(kill(pid, 0) != 0, 3000));
    CT_EQ(stats().deaths, 0);
    bt_link_reset();
}

/* ---- through the hub: RKEY -> config remote -> mapping -> a bt key to a host; held key released when the helper dies ---- */
static int g_hostkeys, g_hostdowns;
static int sink_bt(void *self, uint8_t host, uint8_t page, uint16_t usage, bool down) {
    (void)self;
    g_hostkeys++;
    g_hostdowns += down;
    (void)bt_link_key(host, page, usage, down ? BT_KEY_DOWN : BT_KEY_UP);
    return 0;
}
static const char *HUBCFG =
    "{\"format\":2,\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":233,\"host\":3}},"
    "\"bt_hosts\":[{\"slot\":3,\"addr\":\"AA:BB:CC:DD:EE:01\",\"name\":\"Shield\",\"transport\":\"le\"}],"
    "\"bt_remotes\":[{\"slot\":2,\"addr\":\"11:22:33:44:55:01\",\"name\":\"R\",\"keys\":[[12,233,1]]}]}";
static int g_keyevents;
static void on_bcast(void *c, const char *type, const cJSON *d) {
    (void)c;
    (void)d;
    if (!strcmp(type, "key_event")) g_keyevents++;
}

static void test_hub_integration(void) {
    static hub_ctx_t H;
    static hp_handler_tbl_t T;
    setup("rkey_down");
    hub_ctx_init(&H);
    hub_install(&T, &H); /* installs the engine and the listener */
    bt_link_set_listener(on_listener, NULL);
    static hp_bt_sink bs;
    bs = (hp_bt_sink){NULL, sink_bt};
    hp_sim_t sim;
    hp_sim_init(&sim, NULL);
    H.sinks = hp_sim_sinks(&sim);
    H.sinks.bt = &bs;
    H.bcast_hook = on_bcast;
    CT_EQ(hp_map_load(&H.map, HUBCFG, strlen(HUBCFG)), 0);
    hub_bt_config_sync(&H);
    CT_CHECK(PUMP(g_hostdowns == 1, 4000)); /* RKEY down -> key 1 -> a bt press to host slot 3 */
    CT_CHECK(PUMP(log_count("KEY 0 12 233 1") == 1, 2000)); /* ... which the helper got under ITS slot for that address */
    CT_CHECK(H.map.bt_held.active);
    CT_CHECK(g_keyevents >= 1);
    kill(stats().pid, SIGKILL); /* the helper dies with the key held */
    CT_CHECK(PUMP(stats().deaths == 1, 3000));
    CT_CHECK(!H.map.bt_held.active);
    CT_EQ(g_hostkeys - g_hostdowns, 1); /* the key up went out through the sink (dropped by the dead link) */
    bt_link_reset();
    hp_map_free(&H.map);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_bt_link FAKE_HELPER\n");
        return 2;
    }
    g_helper = argv[1];
    snprintf(g_dir, sizeof g_dir, "/tmp/hh_btlink_%d", (int)getpid());
    mkdir(g_dir, 0700);
    signal(SIGPIPE, SIG_IGN);
    CT_RUN(test_spawn_binding_and_stop);
    CT_RUN(test_death_clears_state);
    CT_RUN(test_remote_keys);
    CT_RUN(test_remote_disconnect);
    CT_RUN(test_backoff_and_cap);
    CT_RUN(test_no_adapter);
    CT_RUN(test_silence_kills);
    CT_RUN(test_ping_keeps_alive);
    CT_RUN(test_malformed_frames);
    CT_RUN(test_partial_frames);
    CT_RUN(test_flood);
    CT_RUN(test_slow_reader);
    CT_RUN(test_bluetoothd_find);
    CT_RUN(test_bluetoothd);
    CT_RUN(test_pairing_windows);
    CT_RUN(test_stubborn_stop);
    CT_RUN(test_hub_integration);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0) {}
    return CT_DONE();
}
