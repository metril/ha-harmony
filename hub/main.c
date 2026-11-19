#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "common/ota/ota_image.h"
#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/io/hw_sinks.h"
#include "hub/io/rf_pair.h"
#include "hub/io/rf_spi.h"
#include "hub/io/sim_sinks.h"
#include "hub/parse_num.h"
#include "hub/net/ident.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/mdns_srv.h"
#include "hub/net/ws.h"

#define REPLY_CAP HP_WS_MAX_FRAME /* a reply that fits is always sendable */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static hp_handler_tbl_t g_tbl;
static hub_ctx_t g_hub;
static hp_sim_t g_sim;
static hp_hw_t g_hw;

static void on_text(hp_ws_conn_t *c, const char *msg, size_t len, void *user) {
    (void)user;
    hp_session_t *sess = hp_ws_conn_user(c);
    if (!sess) {
        sess = calloc(1, sizeof *sess);
        if (!sess) { hp_ws_conn_close(c, 1011); return; }
        hp_ws_conn_set_user(c, sess);
    }
    static char reply[REPLY_CAP];
    size_t rl = 0;
    bool was_authed = sess->authed;
    int r = hp_dispatch(msg, len, &g_tbl, sess, reply, sizeof reply, &rl);
    if (r < 0) { hp_ws_conn_close(c, 1011); return; }
    if (rl && hp_ws_send(c, reply, rl) < 0) { hp_ws_conn_close(c, 1011); return; }
    if (sess->authed) hp_ws_conn_set_authed(c);
    if (!was_authed && sess->authed) hub_send_state(&g_hub, c); /* live state right after hello_ack */
    if (r == HP_DISPATCH_CLOSE) hp_ws_conn_close(c, 1008);
    hub_flush_pending(&g_hub); /* after the reply, so clients see it first */
}

static hp_mdns_srv_t g_mdns; /* fd < 0 until started */

/* set_hub_id changed the id: the mDNS record moves to the new name (goodbye for the old one first). */
static void on_ident_changed(void *ctx, const hp_ident_t *id) {
    (void)ctx;
    char lab[HP_ID_MAX + 1];
    hp_ident_mdns_label(id->id, lab);
    if (g_mdns.sched) {
        if (hp_mdns_srv_rename(&g_mdns, lab) < 0) fprintf(stderr, "hub: mdns keeps the old name (%s is not a DNS label)\n", id->id);
        else snprintf(g_mdns.svc.hub_id, sizeof g_mdns.svc.hub_id, "%s", id->id); /* TXT hub_id= is the real id */
    }
    fprintf(stderr, "hub: id is now %s\n", id->id);
}

/* The ws poll's (max 4) extra wake-only fds: RF pair socket(s) + mDNS. */
static int rf_fds(void *arg, int out[4]) {
    int n = arg ? hp_rf_pair_fds(arg, out) : 0;
    if (n < 0) n = 0;
    int spi = hp_rf_spi_wake_fd(g_hub.sinks.rf); /* the real radio's reader thread wakes the poll */
    if (spi >= 0 && n < 4) out[n++] = spi;
    if (n < 4 && g_mdns.fd >= 0) out[n++] = g_mdns.fd;
    return n;
}

static void on_close(hp_ws_conn_t *c, void *user) {
    (void)user;
    hub_session_closed(&g_hub, hp_ws_conn_user(c));
    free(hp_ws_conn_user(c));
}

static int bad_num(const char *flag, const char *val) {
    fprintf(stderr, "hub: invalid value for %s: '%s'\n", flag, val);
    return 2;
}

#define TOKEN_MAX 256
#define CONF_MAX_LINES 50 /* same bound as rcS.harmony */
static char g_tokbuf[TOKEN_MAX + 1];

/* memset the compiler may not elide (token material). */
static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* Read the WS token from PATH, agreeing with rcS.harmony on a conf: the LAST `token=VALUE` line
 * wins; over-long non-token lines are skipped; an over-long token line, or >50 lines, rejects.
 * With no `token=` line, a bare token as the only non-empty line is accepted. Never prints the
 * value. Returns NULL if unusable. */
static const char *read_token_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    static char iobuf[TOKEN_MAX + 16];
    char line[TOKEN_MAX + 16], first[TOKEN_MAX + 16] = "", val[TOKEN_MAX + 16] = "";
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
    int nonempty = 0, ok = 1, have = 0, at_start = 1, istok = 0, nlines = 0, bare_ok = 0;
    while (ok && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        int full = l && line[l - 1] == '\n';
        if (at_start) {
            if (++nlines > CONF_MAX_LINES) { ok = 0; break; }
            istok = !strncmp(line, "token=", 6);
            if (!full && l == sizeof line - 1) { /* over-long line: skip unless it is the token */
                if (istok) ok = 0;
                else if (nonempty++ == 0) first[0] = 0; /* not a bare token candidate */
                at_start = 0;
                continue;
            }
        } else {
            if (full) at_start = 1;
            else if (l < sizeof line - 1) at_start = 1; /* final chunk at EOF */
            if (istok && ok) ok = 0;
            continue;
        }
        at_start = full || l < sizeof line - 1;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        if (istok) { memcpy(val, line + 6, l - 6 + 1); have = 1; nonempty++; }
        else if (nonempty++ == 0) { memcpy(first, line, l + 1); bare_ok = 1; }
    }
    fclose(f);
    const char *res = NULL;
    if (ok && have) res = val;
    else if (ok && nonempty == 1 && bare_ok) { /* a bare token may end in '=' padding, but `key=value` is another conf line */
        const char *eq = strchr(first, '=');
        if (!eq || !eq[strspn(eq, "=")]) res = first;
    }
    if (res && *res && strlen(res) <= TOKEN_MAX) { strcpy(g_tokbuf, res); } else res = NULL;
    wipe(line, sizeof line); wipe(first, sizeof first); wipe(val, sizeof val); wipe(iobuf, sizeof iobuf);
    return res ? g_tokbuf : NULL;
}

/* Liveness file for the boot hook's watcher: our CLOCK_MONOTONIC seconds (the same scale as
 * /proc/uptime), rewritten in one write(). A wedged main loop stops refreshing it. */
static void heartbeat_touch(const char *path) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld\n", (long long)ts.tv_sec);
    /* tmp + rename: the watcher never sees an empty or half-written file */
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    if (w == n) rename(tmp, path);
    else unlink(tmp);
}

/* The config memory budget for NEW pushes: the asked KiB (hub.conf config_budget_kib), hard-capped at 20% of
 * the reclaimable memory now (MemFree + Buffers + Cached: on the hub's 2.6.31 kernel MemFree alone excludes
 * page cache and tmpfs). A load builds the new map beside the old one, so its peak is up to twice the budget.
 * Re-evaluated at every start and reported in hello_ack.limits; the cache already accepted is never held to
 * it at boot (hp_map_load_cache), so a restart cannot turn an accepted config into "no config". */
static size_t config_budget_bytes(size_t want_kib) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    size_t free_kib = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "MemFree:", 8) || !strncmp(line, "Buffers:", 8) || !strncmp(line, "Cached:", 7))
            free_kib += (size_t)strtoul(line + (line[0] == 'C' ? 7 : 8), NULL, 10);
    if (f) fclose(f);
    size_t cap = free_kib ? free_kib / 5 : want_kib;
    if (cap < 256) cap = 256;
    if (want_kib < 256) { /* below the smallest useful budget: clamp, never refuse to start */
        fprintf(stderr, "hub: config budget %zu KiB raised to the 256 KiB minimum\n", want_kib);
        want_kib = 256;
    }
    size_t kib = want_kib < cap ? want_kib : cap;
    if (kib < want_kib) fprintf(stderr, "hub: config budget capped at %zu KiB (20%% of the %zu KiB free)\n", kib, free_kib);
    return kib * 1024;
}

int main(int argc, char **argv) {
    bool sim = false, rf_spi = false;
    bool rf_debug = false;
    long rf_slot = 0; /* 0 = learn it from the radio's link-up report */
    long port = 8099;
    const char *token = NULL, *token_file = NULL;
    const char *cache = NULL;
    const char *state = NULL;
    const char *rf_path = NULL;
    const char *hub_id = NULL, *fw_version = NULL, *ident_dir = NULL, *hw_id = NULL;
    const char *hb_path = NULL;
    const char *ota_dir = NULL, *ota_target = NULL, *health_file = NULL;
    bool ota_allow_dev = false;
    long hb_secs = 5;
    long budget_kib = HP_MAP_BUDGET_BYTES / 1024; /* --config-budget */
    long ir_ports = HP_HW_DEFAULT_IR_PORTS;
    long auth_ms = HP_WS_AUTH_MS, idle_ms = HP_WS_IDLE_MS;
    int mdns = -1; /* -1 = default: on for the real daemon, off under --sim */
    hp_mdns_opts_t mo = {HP_MDNS_PORT, NULL, NULL, NULL, NULL};
    g_mdns.fd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) { if (hp_parse_num(argv[++i], 0, 65535, &port)) return bad_num("--port", argv[i]); }
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(argv[i], "--token-file") && i + 1 < argc) token_file = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--rf") && i + 1 < argc) rf_path = argv[++i];
        else if (!strcmp(argv[i], "--rf-spi")) rf_spi = true;
        else if (!strcmp(argv[i], "--rf-debug")) rf_debug = true;
        else if (!strcmp(argv[i], "--rf-slot") && i + 1 < argc) {
            char *end = NULL;
            const char *v = argv[++i];
            rf_slot = strtol(v, &end, 10);
            if (!*v || !end || *end || rf_slot < 1 || rf_slot > 15) {
                fprintf(stderr, "hub: --rf-slot %s: must be 1..15 (the remote's HID++ device index)\n", v);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--config-budget") && i + 1 < argc) { if (hp_parse_num(argv[++i], 0, 1048576, &budget_kib)) return bad_num("--config-budget", argv[i]); }
        else if (!strcmp(argv[i], "--heartbeat") && i + 1 < argc) hb_path = argv[++i];
        else if (!strcmp(argv[i], "--heartbeat-secs") && i + 1 < argc) { if (hp_parse_num(argv[++i], 1, 60, &hb_secs)) return bad_num("--heartbeat-secs", argv[i]); }
        else if (!strcmp(argv[i], "--ota-dir") && i + 1 < argc) ota_dir = argv[++i];
        else if (!strcmp(argv[i], "--ota-allow-dev")) ota_allow_dev = true;
        else if (!strcmp(argv[i], "--health-file") && i + 1 < argc) health_file = argv[++i];
        else if (!strcmp(argv[i], "--ota-target") && i + 1 < argc) ota_target = argv[++i];
        else if (!strcmp(argv[i], "--hub-id") && i + 1 < argc) hub_id = argv[++i];
        else if (!strcmp(argv[i], "--ident-dir") && i + 1 < argc) ident_dir = argv[++i];
        else if (!strcmp(argv[i], "--hw-id") && i + 1 < argc) hw_id = argv[++i];
        else if (!strcmp(argv[i], "--fw-version") && i + 1 < argc) fw_version = argv[++i];
        else if (!strcmp(argv[i], "--ir-ports") && i + 1 < argc) { if (hp_parse_num(argv[++i], 1, 255, &ir_ports)) return bad_num("--ir-ports", argv[i]); }
        else if (!strcmp(argv[i], "--mdns")) mdns = 1;
        else if (!strcmp(argv[i], "--no-mdns")) mdns = 0;
        else if (!strcmp(argv[i], "--mdns-port") && i + 1 < argc) { long mp; if (hp_parse_num(argv[++i], 0, 65535, &mp)) return bad_num("--mdns-port", argv[i]); mo.port = (uint16_t)mp; }
        else if (!strcmp(argv[i], "--mdns-addr") && i + 1 < argc) mo.addr = argv[++i];
        else if (!strcmp(argv[i], "--mdns-dest") && i + 1 < argc) mo.dest = argv[++i];
        else if (!strcmp(argv[i], "--auth-timeout-ms") && i + 1 < argc) { if (hp_parse_num(argv[++i], 1, INT32_MAX, &auth_ms)) return bad_num("--auth-timeout-ms", argv[i]); }
        else if (!strcmp(argv[i], "--idle-timeout-ms") && i + 1 < argc) { if (hp_parse_num(argv[++i], 1, INT32_MAX, &idle_ms)) return bad_num("--idle-timeout-ms", argv[i]); }
        else {
            fprintf(stderr, "usage: hub [--sim] [--port N] (--token T | --token-file PATH) [--cache PATH] [--state PATH] [--rf PATH | --rf-spi [--rf-slot N] [--rf-debug]] "
                            "[--ota-dir DIR [--ota-target FILE] [--ota-allow-dev] [--health-file FILE]] [--hub-id ID] [--ident-dir DIR] [--hw-id ID] [--fw-version V] [--heartbeat PATH [--heartbeat-secs N]] [--ir-ports MASK] "
                            "[--config-budget KiB] [--auth-timeout-ms N] [--idle-timeout-ms N] [--mdns|--no-mdns] [--mdns-port N] "
                            "[--mdns-addr A] [--mdns-dest A:P]\n");
            return 2;
        }
    }
    if (!sim) { /* real-hub defaults; --sim keeps none */
        if (!cache) cache = "/data/ha-harmony/cache.json";
        if (!state) state = "/data/ha-harmony/state.json";
        if (!rf_path && !rf_spi) rf_path = "/tmp/ha-harmony-rf.sock";
        if (!ota_dir) ota_dir = "/data/ha-harmony"; /* the dir the boot hook installs the binary in */
    }
    if (rf_spi && (sim || rf_path)) {
        fprintf(stderr, "hub: --rf-spi (the real radio) cannot be combined with --sim or --rf\n");
        return 2;
    }
    if (token_file) {
        if (token) { fprintf(stderr, "hub: --token and --token-file are mutually exclusive\n"); return 2; }
        token = read_token_file(token_file);
    }
    if (!token || !*token || port < 0 || port > 65535 || auth_ms <= 0 || idle_ms <= 0 ||
        auth_ms > INT32_MAX || idle_ms > INT32_MAX) {
        fprintf(stderr, "hub: --token T or a readable --token-file (1-256 chars) required, port 0-65535, timeouts > 0\n");
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: poll returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    hub_ctx_init(&g_hub);
    hp_map_set_budget(&g_hub.map, config_budget_bytes((size_t)budget_kib));
    { /* flash kept free for OTA staging (hub.prev + the new image) and state.json */
        struct stat est;
        size_t bin = (stat("/proc/self/exe", &est) == 0 && est.st_size > 0) ? (size_t)est.st_size : 0;
        hp_cache_set_reserve(bin ? 2 * bin + 256u * 1024u : HP_CACHE_RESERVE_DEFAULT);
    }
    if (sim) {
        hp_sim_init(&g_sim, stdout);
        g_hub.sinks = hp_sim_sinks(&g_sim);
    } else {
        /* a learned slot is kept next to the state file (provisional start value after a restart) */
        char slot_file[256] = "";
        if (rf_slot == 0 && state) {
            const char *sl = strrchr(state, '/');
            int dl = sl ? (int)(sl - state) + 1 : 0;
            if (dl < 200) snprintf(slot_file, sizeof slot_file, "%.*srf_slot", dl, state);
        }
        hp_rf_spi_cfg_t rc_cfg = {NULL, (uint8_t)rf_slot, NULL, HP_RF_SPI_ROLE_HUB, 0, rf_debug, NULL, NULL, slot_file};
        hp_hw_cfg_t hc = {NULL, (uint8_t)ir_ports, NULL, rf_spi ? &rc_cfg : NULL};
        int hr = hp_hw_sinks_open(&hc, &g_hw);
        if (hr == -HP_E_HALBUSY) {
            fprintf(stderr, "hub: stock hal holds the hardware (or could not be ruled out): "
                            "run `tools/deploy_hub.py quiesce` first\n");
            return 1;
        }
        if (hr < 0) {
            fprintf(stderr, "hub: cannot open /dev/i2s (%d)\n", hr);
            return 1;
        }
        g_hub.sinks = g_hw.sinks;
        g_hub.rf_debug = rf_debug;
        g_hub.rf_inner_blocks = rf_spi; /* rf_spi carries bare 28-byte inner blocks; the socket carries hub frames */
    }
    g_hub.cache_path = cache;
    hp_ota_init(&g_hub.ota, ota_dir, ota_target); /* --sim: off unless --ota-dir is given */
    g_hub.ota.allow_dev = ota_allow_dev; /* with the public dev key built in, updates need this explicit opt-in */
    g_hub.ota.health_file = health_file; /* the boot hook's health mark: no update before it exists */
    hp_rf_transport *rf = NULL;
    if (rf_path && strlen(rf_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "hub: --rf path is %zu bytes; a unix socket path must be shorter than %zu bytes (sun_path)\n",
                strlen(rf_path), sizeof(((struct sockaddr_un *)0)->sun_path));
        return 2;
    }
    if (rf_path) {
        if (hp_rf_pair_create(rf_path, &rf) < 0) {
            fprintf(stderr, "hub: cannot listen on RF socket %s\n", rf_path);
            return 1;
        }
        g_hub.sinks.rf = rf;
    }
    if (g_hub.sinks.rf && g_hub.sinks.rf->claim(g_hub.sinks.rf->self) < 0) {
        fprintf(stderr, "hub: RF transport already claimed\n");
        return 1;
    }
    if (cache) {
        int cr = hp_map_load_cache(&g_hub.map, cache);
        if (cr == -HP_E_TOOBIG)
            fprintf(stderr, "hub: cached config %s is over 4x the memory budget (%zu KiB); booting with no config\n", cache,
                    hp_map_budget(&g_hub.map) / 1024);
        else if (cr < 0 && cr != -HP_E_NOENT) fprintf(stderr, "hub: ignoring unusable cache %s (%d)\n", cache, cr);
    }
    hp_actsm_reloaded(&g_hub.act); /* device tracking for the cached config */
    if (state) {
        hp_actsm_set_state_path(&g_hub.act, state);
        int sr = hp_actsm_load_state(&g_hub.act);
        if (sr == -HP_E_NOENT) fprintf(stderr, "hub: no state file %s, starting all off/unknown\n", state);
        else if (sr < 0) fprintf(stderr, "hub: ignoring unusable state file %s (%d), starting all off/unknown\n", state, sr);
        else fprintf(stderr, "hub: restored state from %s\n", state);
    }
    hub_install(&g_tbl, &g_hub);
    g_tbl.token = token;
    static char host[64];
    static char real_id[80];
    if (!sim) {
        if (gethostname(host, sizeof host - 1) != 0 || !host[0]) strcpy(host, "hub");
        snprintf(real_id, sizeof real_id, "ha-harmony-%s", host);
    }
    static char state_dir[256], hw_buf[HP_HW_MAX + 1];
    if (!ident_dir && state) { /* the daemon's own state dir (next to state.json); hub.conf is never written */
        const char *sl = strrchr(state, '/');
        int dl = sl ? (int)(sl - state) : 0;
        if (sl && dl > 0 && dl < (int)sizeof state_dir) {
            snprintf(state_dir, sizeof state_dir, "%.*s", dl, state);
            ident_dir = state_dir;
        }
    }
    if (!hw_id && !sim) {
        hp_ident_hw_read(NULL, hw_buf);
        hw_id = hw_buf;
    }
    (void)hp_ident_init(&g_hub.ident, ident_dir, hub_id, sim ? "sim-hub" : real_id, hw_id); /* never fails */
    if (g_hub.ident.warn[0]) fprintf(stderr, "hub: %s\n", g_hub.ident.warn);
    g_hub.ident_changed = on_ident_changed;
    g_tbl.hub_id = g_hub.ident.id;
    g_tbl.fw_version = fw_version ? fw_version : sim ? "sim" : "ha-harmony-hub";
    fprintf(stderr, "hub: build %u\n", (unsigned)hp_build_id());
    if (ota_pubkey_is_dev()) fprintf(stderr, "hub: dev OTA key built in (never ship this build)\n");
    hp_ws_cb_t cb = {on_text, on_close};
    hp_ws_server_t *srv = hp_ws_listen((uint16_t)port, token, &cb, NULL);
    if (!srv) {
        fprintf(stderr, "hub: cannot listen on port %ld\n", port);
        return 1;
    }
    g_hub.srv = srv;
    if (mdns < 0) mdns = !sim;
    if (mdns) {
        mo.hw = g_hub.ident.hw[0] ? g_hub.ident.hw : NULL;
        char lab[HP_ID_MAX + 1];
        hp_ident_mdns_label(g_tbl.hub_id, lab);
        if (hp_mdns_srv_start(&g_mdns, &g_hub.sched, &mo, lab, hp_ws_port(srv), HP_PROTO_VERSION) < 0)
            fprintf(stderr, "hub: mdns disabled (%s)\n", g_mdns.err);
        else snprintf(g_mdns.svc.hub_id, sizeof g_mdns.svc.hub_id, "%s", g_tbl.hub_id); /* TXT hub_id= is the real id */
    }
    hp_ws_set_extra_fds(srv, rf_fds, rf);
    hp_ws_set_timeouts(srv, (int)auth_ms, (int)idle_ms);
    printf("listening port=%u\n", hp_ws_port(srv));
    if (g_mdns.fd >= 0) printf("mdns port=%u\n", hp_mdns_srv_port(&g_mdns));
    fflush(stdout);
    long long hb_last = -1000;
    int restart_polls = 0; /* applied update: a few more polls to flush the ota_status reply, then exit */
    if (hb_path) heartbeat_touch(hb_path); /* before the loop: the watcher sees us alive at once */
    while (!g_stop) {
        if (hb_path) {
            struct timespec t;
            if (clock_gettime(CLOCK_MONOTONIC, &t) == 0 && (t.tv_sec < hb_last || t.tv_sec - hb_last >= hb_secs)) {
                heartbeat_touch(hb_path);
                hb_last = t.tv_sec;
            }
        }
        int wait = hub_next_timeout_ms(&g_hub); /* timers never sleep: they shorten the poll */
        if (wait < 0 || wait > 200) wait = 200;
        hp_ws_poll(srv, wait);
        hub_pump_rf(&g_hub);
        hp_mdns_srv_service(&g_mdns);
        if (!sim) hp_hw_service(&g_hw);
        hub_run_timers(&g_hub);
        if (g_hub.ota.restart && ++restart_polls >= 3) break;
    }
    hp_mdns_srv_stop(&g_mdns); /* goodbye (TTL 0) */
    hub_shutdown(&g_hub);
    if (!sim) hp_hw_sinks_close(&g_hw);
    hp_ws_close(srv);
    hp_rf_pair_destroy(rf);
    hp_map_free(&g_hub.map);
    return g_hub.ota.restart ? HP_EXIT_OTA_RESTART : 0; /* the boot hook relaunches the new binary */
}
