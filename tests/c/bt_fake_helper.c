/* Test-only stand-in for `hub bt-helper`: speaks hub/bt/bt_ipc.h on fd 3 (or --fd N) with no Bluetooth at all.
 * Behaviour comes from $BT_FAKE_MODE (inherited from the test); every frame the parent sends is appended to $BT_FAKE_LOG
 * as a text line. Modes:
 *   normal        STATE ready, two hosts, two remotes; PONG to PING; answers PAIR / PAIR_REMOTE / FORGET / CONNECT / GET
 *   hang          STATE ready, then never answers anything (no PONG)
 *   crash_start   exits 3 at once      crash_later  normal, exits 1 after 200 ms      noadapter  STATE no_adapter, exit 1
 *   garbage_len   oversized frame length   garbage_type  a parent-direction type   garbage_hosts  HOSTS whose count lies
 *   dribble       normal, but its first frames go out one byte at a time
 *   flood         normal, then 30000 RKEY (a configured and an unconfigured remote) and 200 HOSTS frames as fast as it can
 *   slow          STATE ready, then never reads (the parent's socket fills)
 *   rkey_down     normal + RKEY down (never up) of remote 0 (page 12 usage 0xE9)
 *   rkey          normal + RKEY down/up of remote 0 (page 12 usage 0xE9) and of remote 1 (page 7 usage 0x28)
 *   remote_drop   normal, then REMOTES with remote 0 disconnected after 200 ms
 *   stubborn      normal, ignores SIGTERM */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hub/bt/bt_ipc.h"

static int g_fd = BT_IPC_FD;
static const char *g_mode;
static FILE *g_log;
static int g_dribble;

static void put(const uint8_t *b, size_t n) {
    while (n) {
        size_t chunk = g_dribble ? 1 : n;
        ssize_t w = write(g_fd, b, chunk);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) _exit(0); /* the parent is gone */
        if (g_dribble) usleep(300);
        b += w;
        n -= (size_t)w;
    }
}

static void frame(uint8_t type, const uint8_t *p, size_t n) {
    uint8_t buf[BT_IPC_HDR + BT_IPC_MAX_PAYLOAD];
    put(buf, bt_ipc_frame(buf, type, p, n));
}

static void state(uint8_t st, uint8_t win) {
    uint8_t p[6] = {st, win, 1, 255, 4, 32};
    frame(BT_IPC_STATE, p, sizeof p);
}

static size_t host_entry(uint8_t *o, uint8_t slot, uint8_t tr, uint8_t conn, uint8_t last, const char *name) {
    size_t nl = strlen(name);
    uint8_t e[11] = {slot, tr, 0, conn, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, last, (uint8_t)nl};
    memcpy(o, e, 11);
    memcpy(o + 11, name, nl);
    return 11 + nl;
}

static void hosts(void) {
    uint8_t p[128] = {1, 2};
    size_t n = 2;
    n += host_entry(p + n, 0, BT_TR_LE, 0, 0x01, "Shield");
    n += host_entry(p + n, 5, BT_TR_CLASSIC, 0, 0x05, "Old TV");
    frame(BT_IPC_HOSTS, p, n);
}

static size_t rem_entry(uint8_t *o, uint8_t slot, uint8_t conn, uint8_t bat, uint8_t last, const char *name) {
    size_t nl = strlen(name);
    /* slot, addr_type, connected, battery, fails, addr[6] (11 22 33 44 55 last), name_len */
    uint8_t f[12] = {slot, 1, conn, bat, 0, 0x11, 0x22, 0x33, 0x44, 0x55, last, (uint8_t)nl};
    memcpy(o, f, 12);
    memcpy(o + 12, name, nl);
    return 12 + nl;
}

static void remotes(int r0_connected, int third) {
    uint8_t p[160] = {1, 0};
    size_t n = 2;
    n += rem_entry(p + n, 0, (uint8_t)r0_connected, 80, 0x01, "Remote");
    p[1]++;
    n += rem_entry(p + n, 1, 0, BT_BATTERY_UNKNOWN, 0x02, "");
    p[1]++;
    if (third) {
        n += rem_entry(p + n, 2, 1, 55, 0x09, "Picked");
        p[1]++;
    }
    frame(BT_IPC_REMOTES, p, n);
}

static void rkey(uint8_t slot, uint8_t page, uint16_t usage, uint8_t down) {
    uint8_t p[5] = {slot, page, (uint8_t)(usage >> 8), (uint8_t)usage, down};
    frame(BT_IPC_RKEY, p, sizeof p);
}

static void candidates(void) {
    uint8_t p[14] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x77, 1, (uint8_t)-50, 5, 'C', 'l', 'i', 'c', 'k'};
    frame(BT_IPC_CANDIDATE, p, sizeof p);
    uint8_t q[9] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x88, 1, (uint8_t)-70, 0};
    frame(BT_IPC_CANDIDATE, q, sizeof q);
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void log_line(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void on_frame(uint8_t type, const uint8_t *p, size_t n) {
    switch (type) {
    case BT_IPC_PING:
        if (!strcmp(g_mode, "hang")) return;
        log_line("PING");
        frame(BT_IPC_PONG, p, 1);
        break;
    case BT_IPC_KEY:
        if (n == 5) log_line("KEY %u %u %u %u", p[0], p[1], (p[2] << 8) | p[3], p[4]);
        break;
    case BT_IPC_PAIR:
        if (n != 4 && n != 5) break;
        log_line("PAIR %u %u %u", p[0], (p[1] << 8) | p[2], p[3]);
        if (n == 5) log_line("PAIRTR %u", p[4]);
        state(BT_ST_READY, p[0] ? (uint8_t)(1 + p[3]) : 0);
        if (p[0] && p[3] == 1) candidates();
        break;
    case BT_IPC_PAIR_REMOTE:
        if (n != 7) break;
        log_line("PAIR_REMOTE %02X:%02X:%02X:%02X:%02X:%02X %u", p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
        {
            uint8_t r[11 + 6] = {BT_KIND_REMOTE, BT_PR_OK, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x09, BT_TR_LE, 6};
            memcpy(r + 11, "Picked", 6);
            frame(BT_IPC_PAIRRES, r, sizeof r);
        }
        remotes(1, 1);
        state(BT_ST_READY, 0);
        break;
    case BT_IPC_FORGET:
        if (n == 2) log_line("FORGET %u %u", p[0], p[1]);
        break;
    case BT_IPC_CONNECT:
        if (n == 1) log_line("CONNECT %u", p[0]);
        break;
    case BT_IPC_GET:
        log_line("GET");
        state(BT_ST_READY, 0);
        hosts();
        remotes(1, 0);
        break;
    default: break;
    }
}

static long ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    g_mode = getenv("BT_FAKE_MODE");
    if (!g_mode) g_mode = "normal";
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--fd")) g_fd = atoi(argv[i + 1]);
    const char *lp = getenv("BT_FAKE_LOG");
    if (lp) g_log = fopen(lp, "a");
    signal(SIGPIPE, SIG_IGN);
    if (!strcmp(g_mode, "stubborn")) signal(SIGTERM, SIG_IGN);
    if (!strcmp(g_mode, "crash_start")) _exit(3);
    if (!strcmp(g_mode, "noadapter")) {
        state(BT_ST_NO_ADAPTER, 0);
        _exit(1);
    }
    if (!strcmp(g_mode, "garbage_len")) {
        uint8_t b[3] = {BT_IPC_STATE, 0xFF, 0xFF};
        put(b, sizeof b);
        sleep(30);
        _exit(0);
    }
    if (!strcmp(g_mode, "garbage_type")) {
        uint8_t b[4] = {BT_IPC_PING, 0, 1, 7}; /* a parent-direction type */
        put(b, sizeof b);
        sleep(30);
        _exit(0);
    }
    if (!strcmp(g_mode, "garbage_hosts")) {
        uint8_t p[2] = {1, 9}; /* says 9 entries, has none */
        frame(BT_IPC_HOSTS, p, sizeof p);
        sleep(30);
        _exit(0);
    }
    g_dribble = !strcmp(g_mode, "dribble");
    state(BT_ST_READY, 0);
    hosts();
    remotes(1, 0);
    g_dribble = 0;
    if (!strcmp(g_mode, "hang")) for (;;) sleep(1);
    if (!strcmp(g_mode, "slow")) for (;;) sleep(1);
    if (!strcmp(g_mode, "flood")) {
        for (int i = 0; i < 30000; i++) rkey((uint8_t)(i & 1), 12, 0xE9, (uint8_t)((i >> 1) & 1));
        for (int i = 0; i < 200; i++) hosts();
    }
    long t0 = ms_now();
    int sent_rkey = 0, sent_drop = 0;
    uint8_t buf[2 * (BT_IPC_HDR + BT_IPC_MAX_PAYLOAD)];
    size_t have = 0;
    for (;;) {
        struct pollfd pf = {g_fd, POLLIN, 0};
        if (poll(&pf, 1, 20) > 0) {
            ssize_t r = read(g_fd, buf + have, sizeof buf - have);
            if (r == 0) {
                if (!strcmp(g_mode, "stubborn")) for (;;) sleep(1); /* ignores the parent hanging up, too */
                return 0;
            }
            if (r < 0 && errno != EINTR && errno != EAGAIN) return 0;
            if (r > 0) have += (size_t)r;
            for (;;) {
                uint8_t type = 0;
                const uint8_t *p;
                size_t len = 0;
                size_t used = bt_ipc_parse(buf, have, &type, &p, &len);
                if (used == 0 || used == (size_t)-1) break;
                on_frame(type, p, len);
                memmove(buf, buf + used, have - used);
                have -= used;
            }
        }
        long el = ms_now() - t0;
        if (!strcmp(g_mode, "crash_later") && el > 200) return 1;
        if (!strcmp(g_mode, "rkey") && !sent_rkey && el > 100) {
            sent_rkey = 1;
            rkey(0, 12, 0xE9, 1);
            rkey(1, 7, 0x28, 1);
            rkey(0, 12, 0xE9, 0);
            rkey(1, 7, 0x28, 0);
        }
        if (!strcmp(g_mode, "rkey_down") && !sent_rkey && el > 100) {
            sent_rkey = 1;
            rkey(0, 12, 0xE9, 1);
        }
        if (!strcmp(g_mode, "remote_drop") && !sent_drop && el > 200) {
            sent_drop = 1;
            remotes(0, 0);
        }
    }
}
