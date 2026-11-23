/* hub_log_emit against a real server on loopback: a log line counts as delivered only when it went to a live sink. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hub_handlers.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/ws.h"

static hub_ctx_t H;

/* "A<client>" authenticates the connection as <client> (log sink when "home-assistant"), like a hello would. */
static void on_text(hp_ws_conn_t *c, const char *m, size_t n, void *u) {
    (void)u;
    hp_session_t *s = hp_ws_conn_user(c);
    if (!s) {
        s = calloc(1, sizeof *s);
        hp_ws_conn_set_user(c, s);
    }
    if (n > 1 && m[0] == 'A') {
        s->authed = true;
        s->log_sink = n - 1 == 14 && strncmp(m + 1, "home-assistant", 14) == 0;
        hp_ws_conn_set_authed(c);
    }
}
static void on_close(hp_ws_conn_t *c, void *u) {
    (void)u;
    free(hp_ws_conn_user(c));
}

static void pump(int n) {
    for (int i = 0; i < n; i++) hp_ws_poll(H.srv, 2);
}

static const char REQ[] =
    "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

static int cl_upgrade(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(hp_ws_port(H.srv));
    CT_CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
    send(fd, REQ, sizeof REQ - 1, 0);
    pump(6);
    char b[512];
    CT_CHECK(recv(fd, b, sizeof b, MSG_DONTWAIT) > 0);
    return fd;
}

static void cl_text(int fd, const char *t) {
    static const uint8_t mk[4] = {1, 2, 3, 4};
    uint8_t f[64];
    size_t n = strlen(t);
    f[0] = 0x81;
    f[1] = (uint8_t)(0x80 | n);
    memcpy(f + 2, mk, 4);
    for (size_t i = 0; i < n; i++) f[6 + i] = (uint8_t)(t[i] ^ mk[i & 3]);
    send(fd, f, 6 + n, MSG_NOSIGNAL);
    pump(6);
}

static void test_log_delivered_only_to_live_sinks(void) {
    hp_logline_t ln = {"info", "x", 1, 0};
    CT_CHECK(!hub_log_emit(&H, &ln)); /* nobody connected */
    int c = cl_upgrade();
    cl_text(c, "Aaddon"); /* authed, but not a log sink */
    CT_CHECK(!hub_log_emit(&H, &ln));
    int d = cl_upgrade();
    cl_text(d, "Ahome-assistant");
    CT_CHECK(hub_log_emit(&H, &ln)); /* a live sink took it */
    close(c);
    close(d);
    pump(6);
}

int main(void) {
    hub_ctx_init(&H);
    hp_ws_cb_t cb = {on_text, on_close};
    H.srv = hp_ws_listen(0, "tok", &cb, NULL);
    CT_CHECK(H.srv != NULL);
    test_log_delivered_only_to_live_sinks();
    hp_ws_close(H.srv);
    return CT_DONE();
}
