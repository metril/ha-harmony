/* bt_link for builds with Bluetooth: runs and supervises `hub bt-helper` (hub/bt/bt_ipc.h) and serves the hub/bt_link.h contract from
 * a cached snapshot. Everything runs on the event-loop thread from bt_link_service(); nothing blocks.
 *
 *   helper:  absent -> (wanted) -> defensive bluetoothd kill (once per boot, only if present) -> spawned -> STARTING -> READY
 *   failure: malformed frame / 15 s of silence / exit  -> SIGKILL, cache cleared, engine told, backoff 5/30/120 s, at most
 *            BT_SUP_MAX_RESPAWNS respawns per boot, then status DISABLED. A death never touches the hub's boot-health state.
 *   stop:    no config entries and no window -> SIGTERM, SIGKILL after 2 s; not a failure.
 * Slots: see hub/bt_link.h. The view tables map a reported slot to the helper's own slot by address. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* bt_ipc.h and bt_link.h both define BT_KIND_*: the wire values are BT_IPC_KIND_* here. */
#define BT_KIND_HOST BT_IPC_KIND_HOST
#define BT_KIND_REMOTE BT_IPC_KIND_REMOTE
#include "hub/net/hublog.h"
#include "hub/bt/bt_ipc.h"
#undef BT_KIND_HOST
#undef BT_KIND_REMOTE
#include "hub/bt_link.h"
#include "hub/bt_link_sup.h"
#include "hub/net/ws.h"

#define OUT_CAP 1024
#define RX_CAP (BT_IPC_HDR + BT_IPC_MAX_PAYLOAD)
#define READS_PER_PASS 8
#define UP_RESERVE 256       /* of OUT_CAP: only key UPs may use it (32 frames) */
#define BT_DD_CAP 8          /* remembered dropped DOWNs */
#define RKEYS_PER_PASS 32    /* helper RKEY frames handled per service pass; the rest stays buffered */
#define KILL_GRACE_MS 2000
#define STALL_SLACK_MS 1000 /* a pass later than it asked for by more than this: the loop stalled */
#define NO_SLOT8 0xFF

struct hhost {
    bool used;
    uint8_t slot, transport, connected, addr[6];
    char name[BT_NAME_MAX + 1];
};
struct hrem {
    bool used;
    uint8_t slot, connected, battery, addr_type, addr[6];
    char name[BT_NAME_MAX + 1];
};
struct hcand {
    uint8_t addr[6], addr_type;
    int8_t rssi;
    char name[BT_NAME_MAX + 1];
};
struct cfg {
    bool used, has_addr;
    uint8_t slot, addr[6];
};

static struct {
    struct bt_link_hooks hk;
    /* process */
    pid_t pid, reap;      /* running helper / a killed one not yet reaped */
    int fd;
    int efd;              /* read end of the helper's stderr pipe (its write end is O_NONBLOCK: a stall drops bytes) */
    hp_linebuf_t elb;
    uint64_t kill_at;     /* SIGKILL for `reap` when it has not exited by then */
    uint64_t last_rx, next_ping, next_spawn;
    uint64_t ping_at;     /* when the oldest unanswered PING was sent (0 = none outstanding): the helper is silent only from here */
    uint64_t last_svc;    /* the previous bt_link_service() time: a gap well over the ping period is a stalled main loop, not a quiet helper */
    uint64_t ready_at;    /* since when the helper has been READY (0 = not READY) */
    uint8_t token;
    unsigned deaths, spawns, key_drops, bad_frames;
    bool disabled, io_err;
    bool bz;              /* the stock bluetoothd was looked at this boot */
    /* io */
    uint8_t rx[RX_CAP];
    size_t rx_n;
    uint8_t out[OUT_CAP];
    size_t out_n;
    /* helper's tables */
    struct hhost hh[HH_BT_MAX_HOSTS];
    struct hrem hr[HH_BT_MAX_REMOTES];
    struct hcand cand[BT_LINK_CANDIDATES_CAP];
    uint8_t n_cand;
    bool conn_flag[HH_BT_MAX_REMOTES]; /* remote slot announced connected to the engine */
    uint8_t conn_addr[HH_BT_MAX_REMOTES][6];
    uint8_t hstate, hwin, hle, hhosts_max, hconn_max, hremotes_max;
    bool have_state, last_err;
    /* requests */
    uint8_t dd[BT_DD_CAP][4]; /* slot, page, usage: DOWNs dropped for want of room (their UP is dropped too) */
    unsigned n_dd;
    unsigned rk_pass;     /* RKEY frames handled in this pass */
    bool backlog;         /* complete frames are still buffered: run again at once */
    bool pend_pair;       /* a pair request waiting for the helper to be ready */
    uint8_t pend_mode, pend_tr; /* the pair request that waits for the helper: mode and BT_PAIR_TR_* */
    uint16_t pend_secs;
    enum bt_link_window window; /* optimistic until the helper's STATE says */
    uint64_t win_deadline;      /* host window only */
    uint64_t win_closed_at;     /* when the window last closed (0 = never) */
    /* config + views */
    struct cfg ch[BT_LINK_HOSTS_CAP], cr[BT_LINK_REMOTES_CAP];
    unsigned n_ch, n_cr;
    uint8_t hrep[HH_BT_MAX_HOSTS];  /* helper host slot -> reported slot */
    bool hcfg[HH_BT_MAX_HOSTS];
    uint8_t vh[256];                /* reported host slot -> helper slot */
    uint8_t rrep[HH_BT_MAX_REMOTES];
    bool rcfg[HH_BT_MAX_REMOTES];
    uint8_t vr[HH_BT_MAX_REMOTES];
    struct bt_link_state st;
    bool dirty, rebuild;
    struct bt_link_engine eng;
    void (*listener)(void *);
    void *listener_ctx;
    void (*rkey)(void *, unsigned, unsigned, unsigned, bool);
    void *rkey_ctx;
} L = {.fd = -1, .efd = -1};

/* ------------------------------------------------------------------ helpers */

static uint64_t now_of(void) {
    if (L.hk.now_ms) return L.hk.now_ms();
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000);
}

static void fmt_addr(char out[BT_LINK_ADDR_MAX], const uint8_t a[6]) {
    snprintf(out, BT_LINK_ADDR_MAX, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

static bool parse_addr(const char *s, uint8_t a[6]) {
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        const char *p = s + i * 3;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) || (i < 5 && p[2] != ':')) return false;
        char b[3] = {p[0], p[1], 0};
        a[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    return true;
}

/* A name from the radio is untrusted: bytes up to n, control characters or NUL anywhere -> no name. */
static void copy_name(char *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (src[i] < 0x20 || src[i] == 0x7f) {
            dst[0] = 0;
            return;
        }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool running(void) { return L.fd >= 0; }

/* Config entries, an open or requested pairing window, or a window that closed less than BT_SUP_LINGER_MS ago (a freshly
 * bonded peer that is not in the config yet stays visible so the editor can offer to add it). */
static bool wanted(uint64_t now) {
    return L.n_ch + L.n_cr > 0 || L.window != BT_WIN_NONE || L.pend_pair ||
           (L.win_closed_at && now < L.win_closed_at + BT_SUP_LINGER_MS);
}

static void mark(void) { L.dirty = true; }

/* ------------------------------------------------------------------ views */

static void rebuild(void) {
    L.rebuild = false;
    struct bt_link_state *st = &L.st;
    st->n_hosts = 0;
    st->n_remotes = 0;
    memset(L.hrep, NO_SLOT8, sizeof L.hrep);
    memset(L.vh, NO_SLOT8, sizeof L.vh);
    memset(L.rrep, NO_SLOT8, sizeof L.rrep);
    memset(L.vr, NO_SLOT8, sizeof L.vr);
    memset(L.hcfg, 0, sizeof L.hcfg);
    memset(L.rcfg, 0, sizeof L.rcfg);
    bool taken_h[256] = {false};
    bool taken_r[HH_BT_MAX_REMOTES] = {false};
    for (unsigned i = 0; i < L.n_ch; i++) taken_h[L.ch[i].slot] = true; /* a config slot is never reused for an unconfigured entry */
    for (unsigned i = 0; i < L.n_cr; i++) taken_r[L.cr[i].slot] = true;
    bool bound_h[256] = {false}, bound_r[HH_BT_MAX_REMOTES] = {false};
    /* pass 1: bound by address */
    for (unsigned s = 0; s < HH_BT_MAX_HOSTS; s++) {
        if (!L.hh[s].used) continue;
        for (unsigned i = 0; i < L.n_ch; i++)
            if (L.ch[i].has_addr && !bound_h[L.ch[i].slot] && !memcmp(L.ch[i].addr, L.hh[s].addr, 6)) {
                bound_h[L.ch[i].slot] = true;
                L.hrep[s] = L.ch[i].slot;
                L.hcfg[s] = true;
                break;
            }
    }
    for (unsigned s = 0; s < HH_BT_MAX_REMOTES; s++) {
        if (!L.hr[s].used) continue;
        for (unsigned i = 0; i < L.n_cr; i++)
            if (L.cr[i].has_addr && !bound_r[L.cr[i].slot] && !memcmp(L.cr[i].addr, L.hr[s].addr, 6)) {
                bound_r[L.cr[i].slot] = true;
                L.rrep[s] = L.cr[i].slot;
                L.rcfg[s] = true;
                break;
            }
    }
    /* pass 2: unconfigured entries get a free number (never a config slot) */
    unsigned next = 0;
    for (unsigned s = 0; s < HH_BT_MAX_HOSTS; s++) {
        if (!L.hh[s].used || L.hrep[s] != NO_SLOT8) continue;
        while (next < HH_BT_MAX_HOSTS && taken_h[next]) next++;
        if (next >= HH_BT_MAX_HOSTS) break;
        taken_h[next] = true;
        L.hrep[s] = (uint8_t)next;
    }
    next = 0;
    for (unsigned s = 0; s < HH_BT_MAX_REMOTES; s++) {
        if (!L.hr[s].used || L.rrep[s] != NO_SLOT8) continue;
        while (next < HH_BT_MAX_REMOTES && taken_r[next]) next++;
        if (next >= HH_BT_MAX_REMOTES) break;
        taken_r[next] = true;
        L.rrep[s] = (uint8_t)next;
    }
    for (unsigned s = 0; s < HH_BT_MAX_HOSTS; s++) {
        if (!L.hh[s].used || L.hrep[s] == NO_SLOT8) continue;
        L.vh[L.hrep[s]] = (uint8_t)s;
    }
    for (unsigned s = 0; s < HH_BT_MAX_REMOTES; s++) {
        if (!L.hr[s].used || L.rrep[s] == NO_SLOT8) continue;
        L.vr[L.rrep[s]] = (uint8_t)s;
    }
    /* the snapshot, in reported-slot order */
    for (unsigned r = 0; r < 255 && st->n_hosts < BT_LINK_HOSTS_CAP; r++) {
        if (L.vh[r] == NO_SLOT8) continue;
        const struct hhost *h = &L.hh[L.vh[r]];
        struct bt_link_host *o = &st->hosts[st->n_hosts++];
        memset(o, 0, sizeof *o);
        o->slot = (uint8_t)r;
        fmt_addr(o->addr, h->addr);
        memcpy(o->name, h->name, sizeof o->name < sizeof h->name ? sizeof o->name : sizeof h->name);
        o->name[BT_LINK_NAME_MAX - 1] = 0;
        o->transport = h->transport == BT_TR_LE ? BT_TRANSPORT_LE : BT_TRANSPORT_CLASSIC;
        o->connected = h->connected != 0;
        o->configured = L.hcfg[L.vh[r]];
    }
    for (unsigned r = 0; r < HH_BT_MAX_REMOTES; r++) {
        if (L.vr[r] == NO_SLOT8) continue;
        const struct hrem *h = &L.hr[L.vr[r]];
        struct bt_link_remote *o = &st->remotes[st->n_remotes++];
        memset(o, 0, sizeof *o);
        o->slot = (uint8_t)r;
        fmt_addr(o->addr, h->addr);
        memcpy(o->name, h->name, strlen(h->name) + 1); /* names are <= BT_NAME_MAX bytes */
        o->connected = h->connected != 0;
        o->battery = h->battery <= 100 ? (int8_t)h->battery : -1;
        o->configured = L.rcfg[L.vr[r]];
    }
    /* candidates */
    st->n_candidates = L.n_cand;
    for (unsigned i = 0; i < L.n_cand; i++) {
        fmt_addr(st->candidates[i].addr, L.cand[i].addr);
        memcpy(st->candidates[i].name, L.cand[i].name, strlen(L.cand[i].name) + 1);
        st->candidates[i].rssi = L.cand[i].rssi;
    }
    st->window = L.window;
    st->discoverable = L.window == BT_WIN_HOST;
    st->le = L.hle != 0;
    mark();
}

/* Config slot -> engine for a remote: the engine speaks config slots only. */
static void remotes_lost_check(void) {
    for (unsigned s = 0; s < HH_BT_MAX_REMOTES; s++) {
        bool now_conn = L.hr[s].used && L.hr[s].connected;
        if (L.conn_flag[s] && (!now_conn || memcmp(L.conn_addr[s], L.hr[s].addr, 6) != 0)) {
            L.conn_flag[s] = false;
            for (unsigned i = 0; i < L.n_cr; i++)
                if (L.cr[i].has_addr && !memcmp(L.cr[i].addr, L.conn_addr[s], 6) && L.eng.remote_lost)
                    L.eng.remote_lost(L.eng.ctx, L.cr[i].slot);
        }
        if (now_conn && !L.conn_flag[s]) {
            L.conn_flag[s] = true;
            memcpy(L.conn_addr[s], L.hr[s].addr, 6);
        }
    }
}

/* ------------------------------------------------------------------ process */

/* Forget the last pairing outcome (a new window, a dead helper): a stale "ok" must never be read as the new window's result. */
static void clear_pairing(void) {
    L.st.pairing = BT_PAIR_NONE;
    L.st.pairing_kind = BT_KIND_HOST;
    L.st.pairing_addr[0] = L.st.pairing_name[0] = 0;
    L.st.pairing_transport = BT_TRANSPORT_ANY;
}

static void clear_cache(void) {
    memset(L.hh, 0, sizeof L.hh);
    memset(L.hr, 0, sizeof L.hr);
    L.n_cand = 0;
    L.window = BT_WIN_NONE;
    L.hwin = 0;
    L.have_state = false;
    L.rx_n = 0;
    L.out_n = 0;
    L.n_dd = 0;
    L.backlog = false;
    clear_pairing();
}

/* The helper is gone (or being removed): close the link, tell the engine, clear everything it told us. `pid` keeps being reaped. */
/* The helper's stderr lines: each goes to the log file and the ship ring (hp_log_feed); never blocks. */
static void drain_log(void) {
    char b[256];
    while (L.efd >= 0) {
        ssize_t n = read(L.efd, b, sizeof b);
        if (n > 0) { hp_log_feed(&L.elb, b, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { close(L.efd); L.efd = -1; }
        return; /* EAGAIN: drained */
    }
}

static void close_log(void) {
    drain_log();
    if (L.efd >= 0) close(L.efd);
    L.efd = -1;
    L.elb.n = 0;
    L.elb.over = false;
}

static void drop_link(bool signal_kill, int sig) {
    if (L.fd >= 0) close(L.fd);
    L.fd = -1;
    close_log();
    if (L.pid > 0) {
        if (signal_kill) kill(L.pid, sig);
        L.reap = L.pid; /* a previous unreaped one was already SIGKILLed below */
        L.kill_at = now_of() + KILL_GRACE_MS;
    }
    L.pid = 0;
    L.st.status = L.last_err ? BT_NO_ADAPTER : BT_OFF;
    if (L.eng.release_held) L.eng.release_held(L.eng.ctx);
    /* every connected remote is lost: judged against empty tables */
    memset(L.hr, 0, sizeof L.hr);
    remotes_lost_check();
    clear_cache();
    rebuild();
}

static void failure(const char *why) { /* unexpected death: counts against the budget */
    (void)why;
    drop_link(true, SIGKILL);
    L.ping_at = L.ready_at = 0;
    L.deaths++;
    static const unsigned delay_s[BT_SUP_MAX_RESPAWNS] = {5, 30, 120, 120, 120};
    if (L.deaths > BT_SUP_MAX_RESPAWNS) {
        L.disabled = true;
        L.st.status = BT_DISABLED;
        L.pend_pair = false;
        L.window = BT_WIN_NONE;
        rebuild();
    } else {
        L.next_spawn = now_of() + (uint64_t)delay_s[L.deaths - 1] * 1000u;
    }
    mark();
}

static void stop_helper(void) { /* deliberate: SIGTERM, SIGKILL after the grace period; not a failure */
    drop_link(true, SIGTERM);
    mark();
}

/* The running helper exited by itself (checked after its last bytes were read). */
static void check_exit(void) {
    int st;
    if (L.pid <= 0) return;
    pid_t r = waitpid(L.pid, &st, WNOHANG);
    if (r == L.pid || (r < 0 && errno == ECHILD)) {
        L.pid = 0;
        failure("exited");
    }
}

/* SIGCHLD self-pipe: the handler writes a byte, the poll set carries the read end (bt_link_poll_fd) while no helper socket
 * is open, so a killed helper is reaped when it exits instead of on a timer. */
static int g_chld[2] = {-1, -1};
static void on_sigchld(int sig) {
    (void)sig;
    int e = errno;
    char c = 1;
    ssize_t w = write(g_chld[1], &c, 1);
    (void)w;
    errno = e;
}
static void chld_init(void) {
    if (g_chld[0] >= 0 || pipe2(g_chld, O_CLOEXEC | O_NONBLOCK) < 0) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}
static void chld_drain(void) {
    char b[16];
    if (g_chld[0] >= 0)
        while (read(g_chld[0], b, sizeof b) > 0) {}
}

/* A killed helper still has to be reaped; SIGKILL it if it ignores the first signal. */
static void reap(uint64_t now) {
    int st;
    chld_drain();
    if (L.reap > 0) {
        pid_t r = waitpid(L.reap, &st, WNOHANG);
        if (r == L.reap || (r < 0 && errno == ECHILD)) L.reap = 0;
        else if (now >= L.kill_at) {
            kill(L.reap, SIGKILL);
            L.kill_at = now + KILL_GRACE_MS;
        }
    }
}

/* The process name of /proc/<pid>: comm when the kernel has it (2.6.31 does not), else the 2nd field of stat "(name)", else the
 * basename of the first cmdline word. */
static bool proc_name(const char *root, long pid, char *name, size_t cap) {
    char path[96], buf[512];
    name[0] = 0;
    snprintf(path, sizeof path, "%s/%ld/comm", root, pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(name, (int)cap, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (name[0]) return true;
    }
    snprintf(path, sizeof path, "%s/%ld/stat", root, pid);
    f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        char *o = strchr(buf, '('), *c = strrchr(buf, ')'); /* the name may hold spaces or parentheses: first '(' .. last ')' */
        if (o && c && c > o + 1) {
            snprintf(name, cap, "%.*s", (int)(c - o - 1), o + 1);
            return true;
        }
    }
    snprintf(path, sizeof path, "%s/%ld/cmdline", root, pid);
    f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0; /* argv[0] ends at the first NUL */
        const char *base = strrchr(buf, '/');
        base = base ? base + 1 : buf;
        size_t bl = strlen(base);
        if (bl >= cap) bl = cap ? cap - 1 : 0; /* a name longer than the caller's buffer is cut, never an error */
        memcpy(name, base, bl);
        name[bl] = 0;
        return name[0] != 0;
    }
    return false;
}

/* a zombie or dead process (state Z / X in stat) is not a running daemon */
static bool proc_gone(const char *root, long pid) {
    char path[96], buf[512];
    snprintf(path, sizeof path, "%s/%ld/stat", root, pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *c = strrchr(buf, ')');
    return c && c[1] == ' ' && (c[2] == 'Z' || c[2] == 'X');
}

/* pids of every stock `bluetoothd` under the /proc-like directory root; returns how many (at most max are stored) */
int bt_link_find_bluetoothd(const char *root, long self, long *pids, int max) {
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 1 || pid == self) continue;
        char name[64];
        if (!proc_name(root, pid, name, sizeof name) || strcmp(name, "bluetoothd") != 0) continue;
        if (proc_gone(root, pid)) continue;
        if (n < max) pids[n] = pid;
        n++;
    }
    closedir(d);
    return n;
}

static int default_bluetoothd_signal(int sig) {
    long pids[16];
    int n = bt_link_find_bluetoothd("/proc", (long)getpid(), pids, 16), sent = 0;
    for (int i = 0; i < n && i < 16; i++)
        if (kill((pid_t)pids[i], sig) == 0) sent++;
    return sent;
}

/* The boot hook (tools/boot/rcS.harmony) keeps stock bluetoothd from ever starting while our firmware is enabled, so
 * normally there is nothing here. Defensive only: if one is present anyway (hook mount failed, a manual start) it is
 * SIGKILLed once and the helper spawns at once, with no wait. This kernel (MIPS 2.6.31) has no pidfd and no exit
 * notification for a non-child, and the hub forbids polling; if the dying daemon still holds the controller the helper
 * reports no_adapter and the ordinary spawn backoff retries. */
static void bluetoothd_clear(void) {
    if (L.hk.skip_bluetoothd || L.bz) return;
    L.bz = true;
    int (*sig)(int) = L.hk.bluetoothd_signal ? L.hk.bluetoothd_signal : default_bluetoothd_signal;
    if (sig(0) == 0) return;
    hp_log("hub: bluetooth: stock bluetoothd is running (the boot hook should have prevented it); SIGKILL, spawning without waiting\n");
    sig(SIGKILL);
}

static void spawn(uint64_t now) {
    chld_init();
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) { failure("socketpair"); return; }
    int ep[2];
    if (pipe2(ep, O_CLOEXEC | O_NONBLOCK) < 0) { close(sv[0]); close(sv[1]); failure("pipe"); return; }
    const char *exe = L.hk.helper_exe;
    if (!exe) exe = access("/proc/self/exe", X_OK) == 0 ? "/proc/self/exe" : (L.hk.argv0 ? L.hk.argv0 : "hub");
    char keys[320], hci[16];
    snprintf(keys, sizeof keys, "%s/bt/keys.tlv", L.hk.data_dir ? L.hk.data_dir : "/data/ha-harmony");
    snprintf(hci, sizeof hci, "%u", L.hk.helper_hci);
    char *argv[12];
    int ac = 0;
    argv[ac++] = (char *)exe;
    argv[ac++] = "bt-helper";
    argv[ac++] = "--fd";
    argv[ac++] = "3";
    argv[ac++] = "--keys";
    argv[ac++] = keys;
    if (L.hk.helper_hci) { argv[ac++] = "--hci"; argv[ac++] = hci; }
    argv[ac] = NULL;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); close(ep[0]); close(ep[1]); failure("fork"); return; }
    if (pid == 0) {
        dup2(ep[1], STDERR_FILENO); /* before the IPC dup2: ep[1] may be fd 3; the shared O_NONBLOCK stays */
        if (sv[1] == BT_IPC_FD) fcntl(sv[1], F_SETFD, 0);
        else dup2(sv[1], BT_IPC_FD); /* dup2 clears close-on-exec on the new fd */
        int maxfd = getdtablesize();
        if (maxfd > 4096) maxfd = 4096;
        for (int fd = BT_IPC_FD + 1; fd < maxfd; fd++) close(fd);
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        execv(exe, argv);
        _exit(127);
    }
    close(sv[1]);
    close(ep[1]);
    L.efd = ep[0];
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    L.fd = sv[0];
    L.pid = pid;
    L.spawns++;
    L.last_rx = now;
    L.ping_at = L.ready_at = 0;
    L.next_ping = now + BT_SUP_PING_MS;
    L.rx_n = L.out_n = 0;
    L.st.status = BT_STARTING;
    L.last_err = false;
    mark();
}

/* ------------------------------------------------------------------ output */

static void flush_out(void) {
    while (L.fd >= 0 && L.out_n) {
        ssize_t n = send(L.fd, L.out, L.out_n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) L.io_err = true;
            return;
        }
        if (n == 0) return;
        memmove(L.out, L.out + n, L.out_n - (size_t)n);
        L.out_n -= (size_t)n;
    }
}

/* A control frame: queued (bounded) and written as far as the socket takes it. */
static int send_ctl(uint8_t type, const uint8_t *p, size_t n) {
    if (L.fd < 0) return BT_LINK_E_NOT_READY;
    if (L.out_n + BT_IPC_HDR + n > OUT_CAP - UP_RESERVE) return BT_LINK_E_BUSY; /* the reserve is for key ups */
    L.out_n += bt_ipc_frame(L.out + L.out_n, type, p, n);
    flush_out();
    return BT_LINK_OK;
}

/* A DOWN (or a press = down+up as one unit) is written now or dropped; a partial write's tail is queued so the stream
 * stays in step. A DOWN that was dropped is remembered so its UP is dropped too (never an orphan UP). */
static int send_down(const uint8_t *frames, size_t n, const uint8_t key[4], bool remember) {
    if (L.fd < 0) return BT_LINK_E_NOT_READY;
    flush_out();
    ssize_t w = L.out_n ? -1 : send(L.fd, frames, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w < 0) {
        if (!L.out_n && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) L.io_err = true;
        L.key_drops++;
        if (remember) { /* oldest entry makes room */
            if (L.n_dd == BT_DD_CAP) { memmove(L.dd, L.dd + 1, (BT_DD_CAP - 1) * sizeof L.dd[0]); L.n_dd--; }
            memcpy(L.dd[L.n_dd++], key, 4);
        }
        return BT_LINK_E_BUSY;
    }
    if ((size_t)w < n) {
        memcpy(L.out, frames + w, n - (size_t)w);
        L.out_n = n - (size_t)w;
    }
    return BT_LINK_OK;
}

/* A key UP is never dropped for want of room: it queues in the reserve of the control queue (unless its DOWN was dropped). */
static int send_up(const uint8_t *frame, const uint8_t key[4]) {
    if (L.fd < 0) return BT_LINK_E_NOT_READY;
    for (unsigned i = 0; i < L.n_dd; i++)
        if (!memcmp(L.dd[i], key, 4)) {
            memmove(L.dd[i], L.dd[i + 1], (L.n_dd - i - 1) * sizeof L.dd[0]);
            L.n_dd--;
            return BT_LINK_OK;
        }
    if (L.out_n + BT_IPC_HDR + 5 > OUT_CAP) { L.key_drops++; return BT_LINK_E_BUSY; } /* only if 32 ups are already waiting */
    memcpy(L.out + L.out_n, frame, BT_IPC_HDR + 5);
    L.out_n += BT_IPC_HDR + 5;
    flush_out();
    return BT_LINK_OK;
}

/* ------------------------------------------------------------------ input */

static bool bad(void) { return false; }

/* Sends the pair request that waited for the helper; a full control queue leaves it pending (retried every pass). */
static bool try_pend_pair(void) {
    uint8_t f[5] = {1, (uint8_t)(L.pend_secs >> 8), (uint8_t)L.pend_secs, L.pend_mode, L.pend_tr};
    if (send_ctl(BT_IPC_PAIR, f, sizeof f) != BT_LINK_OK) return false;
    L.pend_pair = false;
    return true;
}

static bool on_state(const uint8_t *p, size_t n) {
    if (n != 6 || p[0] > BT_ST_ERROR || p[1] > 2 || p[2] > 1 || p[5] > HH_BT_MAX_REMOTES) return bad();
    L.hstate = p[0];
    L.hle = p[2];
    L.hhosts_max = p[3];
    L.hconn_max = p[4];
    L.hremotes_max = p[5];
    L.have_state = true;
    enum bt_link_status prev = L.st.status;
    L.last_err = p[0] == BT_ST_NO_ADAPTER || p[0] == BT_ST_ERROR;
    L.st.status = p[0] == BT_ST_READY ? BT_READY : p[0] == BT_ST_STARTING ? BT_STARTING : BT_NO_ADAPTER;
    L.hwin = p[1];
    enum bt_link_window w = p[1] == 1 ? BT_WIN_HOST : p[1] == 2 ? BT_WIN_REMOTE : BT_WIN_NONE;
    (void)prev;
    if (L.st.status != BT_READY) L.ready_at = 0;
    else if (!L.ready_at) L.ready_at = now_of() ? now_of() : 1;
    if (p[0] == BT_ST_READY && L.pend_pair && try_pend_pair()) w = L.pend_mode == BT_MODE_REMOTE ? BT_WIN_REMOTE : BT_WIN_HOST;
    if (w != BT_WIN_REMOTE) L.n_cand = 0;
    if (w == BT_WIN_NONE && L.window != BT_WIN_NONE) L.win_closed_at = now_of();
    L.window = w;
    L.rebuild = true;
    return true;
}

static bool on_pairres(const uint8_t *p, size_t n) {
    if (n < 11 || p[0] > 1 || p[1] > BT_PR_CLOSED || p[10] > BT_NAME_MAX || n != (size_t)11 + p[10]) return bad();
    if (p[1] == BT_PR_TIMEOUT) { L.rebuild = true; return true; } /* the window just ran out: nothing to report */
    L.st.pairing = p[1] == BT_PR_OK ? BT_PAIR_OK : p[1] == BT_PR_FAILED ? BT_PAIR_FAILED : BT_PAIR_REFUSED;
    L.st.pairing_kind = p[0] == BT_IPC_KIND_REMOTE ? BT_KIND_REMOTE : BT_KIND_HOST;
    fmt_addr(L.st.pairing_addr, p + 3);
    L.st.pairing_transport = p[9] == BT_TR_LE ? BT_TRANSPORT_LE : p[9] == BT_TR_CLASSIC ? BT_TRANSPORT_CLASSIC : BT_TRANSPORT_ANY;
    copy_name(L.st.pairing_name, p + 11, p[10]);
    L.rebuild = true;
    return true;
}

static bool on_hosts(const uint8_t *p, size_t n) {
    if (n < 2 || p[1] > BT_IPC_LIST_CHUNK) return bad();
    unsigned cnt = p[1];
    if (p[0] & 1) memset(L.hh, 0, sizeof L.hh);
    size_t o = 2;
    for (unsigned i = 0; i < cnt; i++) {
        if (n - o < 11) return bad();
        const uint8_t *e = p + o;
        uint8_t nl = e[10];
        if (e[0] >= HH_BT_MAX_HOSTS || e[1] > 1 || e[3] > 1 || nl > BT_NAME_MAX || n - o < (size_t)11 + nl) return bad();
        struct hhost *h = &L.hh[e[0]];
        memset(h, 0, sizeof *h);
        h->used = true;
        h->slot = e[0];
        h->transport = e[1];
        h->connected = e[3];
        memcpy(h->addr, e + 4, 6);
        copy_name(h->name, e + 11, nl);
        o += 11 + nl;
    }
    if (o != n) return bad();
    L.rebuild = true;
    return true;
}

static bool on_remotes(const uint8_t *p, size_t n) {
    if (n < 2 || p[1] > BT_IPC_LIST_CHUNK) return bad();
    unsigned cnt = p[1];
    if (p[0] & 1) memset(L.hr, 0, sizeof L.hr);
    size_t o = 2;
    for (unsigned i = 0; i < cnt; i++) {
        if (n - o < 12) return bad();
        const uint8_t *e = p + o;
        uint8_t nl = e[11];
        if (e[0] >= HH_BT_MAX_REMOTES || e[2] > 1 || (e[3] > 100 && e[3] != BT_BATTERY_UNKNOWN) || nl > BT_NAME_MAX ||
            n - o < (size_t)12 + nl)
            return bad();
        struct hrem *h = &L.hr[e[0]];
        memset(h, 0, sizeof *h);
        h->used = true;
        h->slot = e[0];
        h->addr_type = e[1];
        h->connected = e[2];
        h->battery = e[3];
        memcpy(h->addr, e + 5, 6);
        copy_name(h->name, e + 12, nl);
        o += 12 + nl;
    }
    if (o != n) return bad();
    L.rebuild = true;
    return true;
}

static bool on_candidate(const uint8_t *p, size_t n) {
    if (n < 9 || p[8] > BT_NAME_MAX || n != (size_t)9 + p[8]) return bad();
    if (L.window != BT_WIN_REMOTE) return true; /* a late one after the window closed */
    char nm[BT_NAME_MAX + 1];
    copy_name(nm, p + 9, p[8]);
    for (unsigned i = 0; i < L.n_cand; i++)
        if (!memcmp(L.cand[i].addr, p, 6)) {
            L.cand[i].rssi = (int8_t)p[7];
            if (strcmp(L.cand[i].name, nm) != 0) { /* the name arrived: membership-visible change */
                snprintf(L.cand[i].name, sizeof L.cand[i].name, "%s", nm);
                L.rebuild = true;
            } else {
                L.st.candidates[i].rssi = L.cand[i].rssi; /* rssi only: no push */
            }
            return true;
        }
    if (L.n_cand >= BT_LINK_CANDIDATES_CAP) return true;
    struct hcand *c = &L.cand[L.n_cand++];
    memcpy(c->addr, p, 6);
    c->addr_type = p[6];
    c->rssi = (int8_t)p[7];
    snprintf(c->name, sizeof c->name, "%s", nm);
    L.rebuild = true;
    return true;
}

static bool on_rkey(const uint8_t *p, size_t n) {
    if (n != 5 || p[0] >= HH_BT_MAX_REMOTES || (p[1] != BT_PAGE_KEYBOARD && p[1] != BT_PAGE_CONSUMER) || p[4] > 1) return bad();
    unsigned s = p[0];
    if (L.rebuild) rebuild(); /* a REMOTES frame just before this key: bind it first */
    if (!L.hr[s].used) return true;
    unsigned page = p[1], usage = ((unsigned)p[2] << 8) | p[3];
    bool down = p[4] != 0;
    if (L.rcfg[s] && L.eng.remote_key) {
        for (unsigned i = 0; i < L.n_cr; i++)
            if (L.cr[i].has_addr && !memcmp(L.cr[i].addr, L.hr[s].addr, 6)) {
                L.eng.remote_key(L.eng.ctx, L.cr[i].slot, page, usage, down);
                break;
            }
    }
    if (L.rkey && L.rrep[s] != NO_SLOT8) L.rkey(L.rkey_ctx, L.rrep[s], page, usage, down);
    return true;
}

static bool on_frame(uint8_t type, const uint8_t *p, size_t n) {
    switch (type) {
    case BT_IPC_PONG: return n == 1 ? true : bad();
    case BT_IPC_STATE: return on_state(p, n);
    case BT_IPC_PAIRRES: return on_pairres(p, n);
    case BT_IPC_HOSTS: return on_hosts(p, n);
    case BT_IPC_REMOTES: return on_remotes(p, n);
    case BT_IPC_RKEY: return on_rkey(p, n);
    case BT_IPC_CANDIDATE: return on_candidate(p, n);
    default: return type >= 0x80 ? true : bad(); /* a newer helper's frame is ignored; a parent-direction type is nonsense */
    }
}

/* Reads and parses what the socket holds (bounded per pass). Returns false when the helper must be killed. */
/* Parses the buffered frames; stops after RKEYS_PER_PASS key frames (backlog set, the rest stays buffered). */
static bool parse_buf(uint64_t now) {
    size_t off = 0;
    bool ok = true;
    for (;;) {
        uint8_t type;
        const uint8_t *pl;
        size_t len;
        size_t used = bt_ipc_parse(L.rx + off, L.rx_n - off, &type, &pl, &len);
        if (used == (size_t)-1) { L.bad_frames++; ok = false; break; }
        if (used == 0) break;
        if (type == BT_IPC_RKEY && L.rk_pass >= RKEYS_PER_PASS) { L.backlog = true; break; }
        if (type == BT_IPC_RKEY) L.rk_pass++;
        L.last_rx = now;
        L.ping_at = 0; /* any frame proves the helper is alive and reading */
        if (!on_frame(type, pl, len)) { L.bad_frames++; ok = false; break; }
        off += used;
    }
    if (off) {
        memmove(L.rx, L.rx + off, L.rx_n - off);
        L.rx_n -= off;
    }
    return ok;
}

static bool read_in(uint64_t now) {
    L.rk_pass = 0;
    L.backlog = false;
    if (!parse_buf(now)) return false; /* what the last pass left */
    for (int r = 0; r < READS_PER_PASS && !L.backlog; r++) {
        size_t space = sizeof L.rx - L.rx_n;
        ssize_t n = recv(L.fd, L.rx + L.rx_n, space, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        if (n == 0) return false; /* EOF: the helper closed its end (the reaper handles the exit) */
        L.rx_n += (size_t)n;
        if (!parse_buf(now)) return false;
        if ((size_t)n < space) break;
    }
    return true;
}

/* ------------------------------------------------------------------ public */

void bt_link_configure(const struct bt_link_hooks *h) { L.hk = *h; }

void bt_link_stats(struct bt_link_stats *o) {
    o->key_drops = L.key_drops;
    o->deaths = L.deaths;
    o->spawns = L.spawns;
    o->bad_frames = L.bad_frames;
    o->pid = L.pid;
}

void bt_link_reset(void) {
    if (L.fd >= 0) close(L.fd);
    if (L.efd >= 0) close(L.efd);
    if (L.pid > 0) { kill(L.pid, SIGKILL); waitpid(L.pid, NULL, 0); }
    if (L.reap > 0) { kill(L.reap, SIGKILL); waitpid(L.reap, NULL, 0); }
    memset(&L, 0, sizeof L);
    L.fd = -1;
    L.efd = -1;
}

bool bt_link_available(void) { return !L.hk.unavailable; }

void bt_link_limits(struct bt_link_limits *o) {
    memset(o, 0, sizeof *o);
    o->hosts_max = L.have_state ? L.hhosts_max : HH_BT_MAX_HOSTS;
    o->conn_max = L.have_state ? L.hconn_max : HH_BT_MAX_CONN;
    o->remotes_max = L.have_state ? L.hremotes_max : HH_BT_MAX_REMOTES;
#ifdef HH_BT_LE
    o->le = L.have_state ? L.hle != 0 : true;
#else
    o->le = false;
#endif
    o->le_bonds_max = (uint16_t)((o->le ? o->hosts_max : 0) + o->remotes_max);
}

void bt_link_state(struct bt_link_state *out) {
    struct bt_link_state *s = &L.st;
    s->discoverable_secs = 0;
    if (L.window == BT_WIN_HOST && L.win_deadline) {
        uint64_t now = now_of();
        s->discoverable_secs = L.win_deadline > now ? (uint16_t)((L.win_deadline - now + 999) / 1000) : 0;
    }
    memcpy(out, s, sizeof *out);
}

void bt_link_set_listener(void (*cb)(void *), void *ctx) {
    L.listener = cb;
    L.listener_ctx = ctx;
}

void bt_link_set_remote_key_cb(void (*cb)(void *, unsigned, unsigned, unsigned, bool), void *ctx) {
    L.rkey = cb;
    L.rkey_ctx = ctx;
}

void bt_link_set_engine(const struct bt_link_engine *e) {
    if (e) L.eng = *e;
    else memset(&L.eng, 0, sizeof L.eng);
}

static void load_cfg(struct cfg *dst, unsigned *n, const struct bt_link_cfg_entry *src, unsigned ns, unsigned cap) {
    *n = 0;
    for (unsigned i = 0; i < ns && *n < cap; i++) {
        struct cfg *c = &dst[(*n)++];
        memset(c, 0, sizeof *c);
        c->used = true;
        c->slot = src[i].slot;
        c->has_addr = parse_addr(src[i].addr, c->addr);
    }
}

void bt_link_set_config(const struct bt_link_cfg_entry *hosts, unsigned nh, const struct bt_link_cfg_entry *remotes,
                        unsigned nr) {
    load_cfg(L.ch, &L.n_ch, hosts, nh, BT_LINK_HOSTS_CAP);
    load_cfg(L.cr, &L.n_cr, remotes, nr, BT_LINK_REMOTES_CAP);
    rebuild();
}

int bt_link_pair(bool on, unsigned secs, enum bt_link_transport transport, enum bt_link_pair_mode mode) {
    if (L.disabled) return BT_LINK_E_NOT_READY;
    if (!on) {
        L.pend_pair = false;
        if (L.window != BT_WIN_NONE && L.st.status == BT_READY) {
            uint8_t f[4] = {0, 0, 0, (uint8_t)mode};
            int rc = send_ctl(BT_IPC_PAIR, f, sizeof f);
            if (rc != BT_LINK_OK) return rc;
        }
        if (L.window != BT_WIN_NONE) L.win_closed_at = now_of();
        L.window = BT_WIN_NONE;
        L.n_cand = 0;
        rebuild();
        return BT_LINK_OK;
    }
    if (L.st.status == BT_NO_ADAPTER) return BT_LINK_E_NOT_READY;
    uint8_t m = mode == BT_MODE_REMOTE ? 1 : 0;
    /* which radio takes a new host bond (a remote window is always LE; the helper ignores it there) */
    uint8_t tr = transport == BT_TRANSPORT_CLASSIC ? BT_PAIR_TR_CLASSIC : transport == BT_TRANSPORT_LE ? BT_PAIR_TR_LE : BT_PAIR_TR_ANY;
    clear_pairing(); /* rebuild() below bumps gen and fires the listener, so a poller sees the result go */
    L.n_cand = 0;
    if (L.st.status == BT_READY) {
        uint8_t f[5] = {1, (uint8_t)(secs >> 8), (uint8_t)secs, m, tr};
        int rc = send_ctl(BT_IPC_PAIR, f, sizeof f);
        if (rc != BT_LINK_OK) return rc;
    } else {
        L.pend_pair = true; /* sent when the helper reports ready */
        L.pend_mode = m;
        L.pend_tr = tr;
        L.pend_secs = (uint16_t)secs;
        if (!running() && L.st.status == BT_OFF) L.st.status = BT_STARTING;
    }
    L.window = m ? BT_WIN_REMOTE : BT_WIN_HOST;
    L.win_deadline = m ? 0 : now_of() + (uint64_t)secs * 1000u;
    rebuild();
    return BT_LINK_OK;
}

int bt_link_pair_remote(const char *addr) {
    uint8_t a[6];
    if (L.st.status != BT_READY) return BT_LINK_E_NOT_READY;
    if (!parse_addr(addr, a)) return BT_LINK_E_NO_SLOT;
    for (unsigned i = 0; i < L.n_cand; i++)
        if (!memcmp(L.cand[i].addr, a, 6)) {
            uint8_t f[7];
            memcpy(f, a, 6);
            f[6] = L.cand[i].addr_type;
            return send_ctl(BT_IPC_PAIR_REMOTE, f, sizeof f);
        }
    return BT_LINK_E_NO_SLOT;
}

int bt_link_forget(enum bt_link_kind kind, unsigned slot) {
    if (L.st.status != BT_READY) return BT_LINK_E_NOT_READY;
    if (slot >= 256) return BT_LINK_E_NO_SLOT;
    uint8_t hs = kind == BT_KIND_REMOTE ? (slot < HH_BT_MAX_REMOTES ? L.vr[slot] : NO_SLOT8) : L.vh[slot];
    if (hs == NO_SLOT8) return BT_LINK_E_NO_SLOT;
    uint8_t f[2] = {kind == BT_KIND_REMOTE ? BT_IPC_KIND_REMOTE : BT_IPC_KIND_HOST, hs};
    return send_ctl(BT_IPC_FORGET, f, sizeof f);
}

int bt_link_key(unsigned slot, unsigned page, unsigned usage, enum bt_link_action action) {
    if (L.st.status != BT_READY) return BT_LINK_E_NOT_READY;
    if (slot >= 256 || L.vh[slot] == NO_SLOT8) return BT_LINK_E_NO_SLOT;
    uint8_t f[2 * (BT_IPC_HDR + 5)];
    size_t n = 0;
    for (int d = 0; d < (action == BT_KEY_PRESS ? 2 : 1); d++) {
        bool down = action == BT_KEY_DOWN || (action == BT_KEY_PRESS && d == 0);
        uint8_t p[5] = {L.vh[slot], (uint8_t)page, (uint8_t)(usage >> 8), (uint8_t)usage, down ? 1 : 0};
        n += bt_ipc_frame(f + n, BT_IPC_KEY, p, sizeof p);
    }
    const uint8_t key[4] = {L.vh[slot], (uint8_t)page, (uint8_t)(usage >> 8), (uint8_t)usage};
    if (action == BT_KEY_UP) return send_up(f, key);
    return send_down(f, n, key, action == BT_KEY_DOWN);
}

void bt_link_connect(unsigned slot) {
    if (L.st.status != BT_READY || slot >= 256 || L.vh[slot] == NO_SLOT8) return;
    uint8_t s = L.vh[slot];
    if (L.hh[s].connected) return;
    (void)send_ctl(BT_IPC_CONNECT, &s, 1);
}

/* The helper's socket; while none is open, the SIGCHLD pipe (a killed helper to reap). */
_Static_assert(BT_LINK_POLL_OUT == HP_WS_FD_OUT, "poll flag agrees with ws");
int bt_link_poll_fd(void) {
    if (L.fd < 0) return g_chld[0];
    return L.out_n ? (L.fd | BT_LINK_POLL_OUT) : L.fd; /* output waits for the socket to drain: POLLOUT, not a timer */
}
int bt_link_log_fd(void) { return L.efd; }

/* The real time to `t`; a huge one is clamped only to fit an int. */
static int ms_until(uint64_t now, uint64_t t) { return t <= now ? 0 : (t - now > 0x7fffffffULL ? 0x7fffffff : (int)(t - now)); }

int bt_link_service(void) {
    uint64_t now = now_of();
    int next = -1;
#define WAKE(t) do { int w_ = ms_until(now, (t)); if (next < 0 || w_ < next) next = w_; } while (0)
    drain_log();
    reap(now);
    if (running()) {
        flush_out();
        bool ok_in = read_in(now); /* a PONG already waiting in the socket clears ping_at here */
        /* The loop itself stalled since the last pass: a PING may only just have been flushed, so the helper gets a fresh window. */
        if (L.ping_at && L.last_svc && now - L.last_svc > BT_SUP_PING_MS + STALL_SLACK_MS) L.ping_at = now; /* wakes now come exactly at the ping due time, so the period alone is no stall */
        if (!ok_in || L.io_err) {
            L.io_err = false;
            failure(L.bad_frames ? "malformed frame" : "link closed");
        } else if (check_exit(), !running()) {
            /* exited; failure() ran */
        } else if (L.ping_at && now - L.ping_at >= BT_SUP_SILENCE_MS) {
            failure("silent");
        } else if (!wanted(now)) {
            stop_helper();
        } else if (now >= L.next_ping) {
            uint8_t t = ++L.token;
            send_ctl(BT_IPC_PING, &t, 1);
            if (!L.ping_at) L.ping_at = now ? now : 1;
            L.next_ping = now + BT_SUP_PING_MS;
        }
    }
    L.last_svc = now ? now : 1;
    if (L.deaths && L.ready_at && running() && L.st.status == BT_READY && now - L.ready_at >= BT_SUP_HEALTHY_MS) L.deaths = 0; /* a healthy stretch forgives earlier crashes */
    if (!running() && !L.disabled && wanted(now) && L.reap == 0 && now >= L.next_spawn) {
        bluetoothd_clear();
        spawn(now);
    }
    if (L.rebuild) {
        rebuild();
        remotes_lost_check();
    }
    if (L.dirty) {
        L.dirty = false;
        L.st.gen++;
        if (L.listener) L.listener(L.listener_ctx);
    }
    if (running() && L.pend_pair && L.st.status == BT_READY && try_pend_pair()) L.rebuild = true;
    if (running()) {
        if (L.backlog) WAKE(now); /* buffered frames wait for the next pass */
        /* pend_pair needs no timer: it waits for the helper's READY state (socket readable) or, when the control
         * queue is full, for the socket to drain (POLLOUT, below) */
        WAKE(L.next_ping);
        if (L.ping_at) WAKE(L.ping_at + BT_SUP_SILENCE_MS);
        if (L.win_closed_at && !(L.n_ch + L.n_cr > 0 || L.window != BT_WIN_NONE || L.pend_pair)) WAKE(L.win_closed_at + BT_SUP_LINGER_MS); /* the linger ends: stop the helper then */
    } else if (!L.disabled && wanted(now)) {
        if (L.reap == 0) WAKE(L.next_spawn);
    }
    if (L.reap > 0) WAKE(L.kill_at); /* SIGCHLD wakes the poll (bt_link_poll_fd); this is the SIGKILL deadline */
#undef WAKE
    return next;
}
