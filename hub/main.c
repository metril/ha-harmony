#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/hp_errors.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/ws.h"

#define REPLY_CAP HP_WS_MAX_FRAME /* a reply that fits is always sendable */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static hp_handler_tbl_t g_tbl;

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
    int r = hp_dispatch(msg, len, &g_tbl, sess, reply, sizeof reply, &rl);
    if (r < 0) { hp_ws_conn_close(c, 1011); return; }
    if (rl && hp_ws_send(c, reply, rl) < 0) { hp_ws_conn_close(c, 1011); return; }
    if (r == HP_DISPATCH_CLOSE) hp_ws_conn_close(c, 1008);
}

static void on_close(hp_ws_conn_t *c, void *user) {
    (void)user;
    free(hp_ws_conn_user(c));
}

int main(int argc, char **argv) {
    bool sim = false;
    long port = 8099;
    const char *token = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
        else {
            fprintf(stderr, "usage: hub --sim [--port N] --token T\n");
            return 2;
        }
    }
    if (!sim) {
        fprintf(stderr, "no hardware backend built\n");
        return 1;
    }
    if (!token || !*token || port < 0 || port > 65535) {
        fprintf(stderr, "hub: --token T required, port 0-65535\n");
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: poll returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_tbl.token = token;
    g_tbl.hub_id = "sim-hub";
    g_tbl.fw_version = "sim";
    hp_ws_cb_t cb = {on_text, on_close};
    hp_ws_server_t *srv = hp_ws_listen((uint16_t)port, token, &cb, NULL);
    if (!srv) {
        fprintf(stderr, "hub: cannot listen on port %ld\n", port);
        return 1;
    }
    printf("listening port=%u\n", hp_ws_port(srv));
    fflush(stdout);
    while (!g_stop) hp_ws_poll(srv, 200);
    hp_ws_close(srv);
    return 0;
}
