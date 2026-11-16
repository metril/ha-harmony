#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/io/rf_pair.h"
#include "hub/io/sim_sinks.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/ws.h"

#define REPLY_CAP HP_WS_MAX_FRAME /* a reply that fits is always sendable */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static hp_handler_tbl_t g_tbl;
static hub_ctx_t g_hub;
static hp_sim_t g_sim;

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

static int rf_fds(void *arg, int out[2]) { return hp_rf_pair_fds(arg, out); }

static void on_close(hp_ws_conn_t *c, void *user) {
    (void)user;
    free(hp_ws_conn_user(c));
}

int main(int argc, char **argv) {
    bool sim = false;
    long port = 8099;
    const char *token = NULL;
    const char *cache = NULL;
    const char *rf_path = NULL;
    long auth_ms = HP_WS_AUTH_MS, idle_ms = HP_WS_IDLE_MS;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--rf") && i + 1 < argc) rf_path = argv[++i];
        else if (!strcmp(argv[i], "--auth-timeout-ms") && i + 1 < argc) auth_ms = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--idle-timeout-ms") && i + 1 < argc) idle_ms = strtol(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "usage: hub --sim [--port N] --token T [--cache PATH] [--rf PATH] "
                            "[--auth-timeout-ms N] [--idle-timeout-ms N]\n");
            return 2;
        }
    }
    if (!sim) {
        fprintf(stderr, "no hardware backend built\n");
        return 1;
    }
    if (!token || !*token || port < 0 || port > 65535 || auth_ms <= 0 || idle_ms <= 0 ||
        auth_ms > INT32_MAX || idle_ms > INT32_MAX) {
        fprintf(stderr, "hub: --token T required, port 0-65535, timeouts > 0\n");
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: poll returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    hub_ctx_init(&g_hub);
    hp_sim_init(&g_sim, stdout);
    g_hub.sinks = hp_sim_sinks(&g_sim);
    g_hub.cache_path = cache;
    hp_rf_transport *rf = NULL;
    if (rf_path) {
        if (hp_rf_pair_create(rf_path, &rf) < 0) {
            fprintf(stderr, "hub: cannot listen on RF socket %s\n", rf_path);
            return 1;
        }
        g_hub.sinks.rf = rf;
    }
    if (g_hub.sinks.rf->claim(g_hub.sinks.rf->self) < 0) {
        fprintf(stderr, "hub: RF transport already claimed\n");
        return 1;
    }
    if (cache) {
        int cr = hp_map_load_cache(&g_hub.map, cache);
        if (cr < 0 && cr != -HP_E_NOENT) fprintf(stderr, "hub: ignoring unusable cache %s (%d)\n", cache, cr);
    }
    hub_install(&g_tbl, &g_hub);
    g_tbl.token = token;
    g_tbl.hub_id = "sim-hub";
    g_tbl.fw_version = "sim";
    hp_ws_cb_t cb = {on_text, on_close};
    hp_ws_server_t *srv = hp_ws_listen((uint16_t)port, token, &cb, NULL);
    if (!srv) {
        fprintf(stderr, "hub: cannot listen on port %ld\n", port);
        return 1;
    }
    g_hub.srv = srv;
    if (rf) hp_ws_set_extra_fds(srv, rf_fds, rf);
    hp_ws_set_timeouts(srv, (int)auth_ms, (int)idle_ms);
    printf("listening port=%u\n", hp_ws_port(srv));
    fflush(stdout);
    while (!g_stop) {
        hp_ws_poll(srv, 200);
        hub_pump_rf(&g_hub);
    }
    hp_ws_close(srv);
    hp_rf_pair_destroy(rf);
    hp_map_free(&g_hub.map);
    return 0;
}
