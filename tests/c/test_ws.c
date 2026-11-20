#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/net/ws.h"

/* ---------- server-side recording ---------- */
static char g_text[70000];
static size_t g_len;
static int g_texts, g_closes;
static hp_ws_conn_t *g_conn;

static void on_text(hp_ws_conn_t *c, const char *m, size_t n, void *u) {
    (void)u;
    g_conn = c;
    memcpy(g_text, m, n);
    g_len = n;
    g_texts++;
}
static void on_close(hp_ws_conn_t *c, void *u) { (void)c; (void)u; g_closes++; }

static hp_ws_server_t *srv;

static void pump(int n) {
    for (int i = 0; i < n; i++) hp_ws_poll(srv, 2);
}

static int cl_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(hp_ws_port(srv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* no Nagle delay between frames */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static ssize_t cl_read(int fd, uint8_t *buf, size_t cap) {
    pump(6);
    size_t tot = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + tot, cap - tot, MSG_DONTWAIT);
        if (n <= 0) break;
        tot += (size_t)n;
    }
    return (ssize_t)tot;
}

static const char REQ[] =
    "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

static int cl_upgrade(void) {
    int fd = cl_connect();
    send(fd, REQ, sizeof REQ - 1, 0);
    uint8_t b[512];
    ssize_t n = cl_read(fd, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    CT_CHECK(n > 0 && strstr((char *)b, "101 Switching") && strstr((char *)b, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    return fd;
}

/* Builds a masked client frame; returns its length. */
static size_t mkframe(uint8_t *o, uint8_t op, bool fin, const uint8_t *p, size_t n, bool mask) {
    static const uint8_t mk[4] = {0x37, 0xfa, 0x21, 0x3d};
    size_t h = 0;
    o[h++] = (uint8_t)((fin ? 0x80 : 0) | op);
    uint8_t mb = mask ? 0x80 : 0;
    if (n < 126) o[h++] = (uint8_t)(mb | n);
    else if (n < 65536) { o[h++] = mb | 126; o[h++] = (uint8_t)(n >> 8); o[h++] = (uint8_t)n; }
    else { o[h++] = mb | 127; for (int i = 7; i >= 0; i--) o[h++] = (uint8_t)((uint64_t)n >> (8 * i)); }
    if (mask) { memcpy(o + h, mk, 4); h += 4; }
    for (size_t i = 0; i < n; i++) o[h + i] = mask ? (uint8_t)(p[i] ^ mk[i & 3]) : p[i];
    return h + n;
}

static uint8_t big[70000 + 16];

static void send_frame(int fd, uint8_t op, bool fin, const uint8_t *p, size_t n) {
    size_t l = mkframe(big, op, fin, p, n, true);
    send(fd, big, l, MSG_NOSIGNAL);
}

static int close_code(const uint8_t *b, ssize_t n) {
    return (n >= 4 && (b[0] & 0x0F) == 8 && b[1] == 2) ? (b[2] << 8) | b[3] : -1;
}

static bool peer_closed(int fd) {
    uint8_t b;
    pump(6);
    return recv(fd, &b, 1, MSG_DONTWAIT) == 0;
}

/* ---------- pure helpers ---------- */
static void test_helpers(void) {
    char acc[29];
    hp_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", 24, acc);
    CT_CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    uint8_t d[20];
    hp_sha1((const uint8_t *)"abc", 3, d);
    CT_EQ(d[0], 0xa9); CT_EQ(d[19], 0x9d);
    char b64[16];
    hp_base64_encode((const uint8_t *)"fo", 2, b64);
    CT_CHECK(strcmp(b64, "Zm8=") == 0);
    CT_CHECK(hp_base64_valid("Zm8=", 4) && hp_base64_valid("", 0));
    CT_CHECK(!hp_base64_valid("Zm8", 3) && !hp_base64_valid("Z=8=", 4) && !hp_base64_valid("Zm!=", 4));
    CT_CHECK(hp_utf8_valid((const uint8_t *)"h\xc3\xa9llo", 6));
    CT_CHECK(!hp_utf8_valid((const uint8_t *)"\xc3", 1));          /* truncated */
    CT_CHECK(!hp_utf8_valid((const uint8_t *)"\xc0\xaf", 2));      /* overlong */
    CT_CHECK(!hp_utf8_valid((const uint8_t *)"\xed\xa0\x80", 3));  /* surrogate */
    CT_CHECK(!hp_utf8_valid((const uint8_t *)"\xf4\x90\x80\x80", 4));
}

static void test_handshake_parse(void) {
    size_t used = 0;
    char acc[29];
    CT_EQ(hp_ws_parse_handshake(REQ, sizeof REQ - 1, &used, acc), HP_WS_HS_OK);
    CT_EQ(used, sizeof REQ - 1);
    CT_EQ(hp_ws_parse_handshake(REQ, 20, &used, acc), HP_WS_HS_NEED_MORE);
    const char nokey[] = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n";
    CT_EQ(hp_ws_parse_handshake(nokey, sizeof nokey - 1, &used, acc), -400);
    const char post[] = "POST / HTTP/1.1\r\n\r\n";
    CT_EQ(hp_ws_parse_handshake(post, sizeof post - 1, &used, acc), -400);
    const char v8[] = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n";
    CT_EQ(hp_ws_parse_handshake(v8, sizeof v8 - 1, &used, acc), -426);
    char huge[9000];
    memset(huge, 'a', sizeof huge);
    memcpy(huge, "GET ", 4);
    CT_EQ(hp_ws_parse_handshake(huge, sizeof huge, &used, acc), -431);
}

static void test_frame_hdr(void) {
    uint8_t b[16];
    uint8_t pl[126] = {0};
    size_t l = mkframe(big, 1, true, pl, 126, true);
    hp_ws_frame_hdr_t h;
    CT_EQ(hp_ws_parse_frame_hdr(big, l, &h), HP_WS_FRAME_OK);
    CT_EQ(h.payload_len, 126);
    CT_EQ(h.hdr_len, 8);
    CT_EQ(hp_ws_parse_frame_hdr(big, 3, &h), HP_WS_FRAME_NEED_MORE);
    l = mkframe(b, 1, true, NULL, 0, false);
    CT_EQ(hp_ws_parse_frame_hdr(b, l, &h), -1002); /* unmasked */
    memset(b, 0, sizeof b);
    b[0] = 0x81; b[1] = 0xFF; b[2] = 0x80; /* 64-bit length with top bit set */
    CT_EQ(hp_ws_parse_frame_hdr(b, 14, &h), -1009);
    b[2] = 0; b[7] = 1; b[9] = 1; /* 65537 */
    CT_EQ(hp_ws_parse_frame_hdr(b, 14, &h), -1009);
    b[0] = 0xC1; b[1] = 0x80;
    CT_EQ(hp_ws_parse_frame_hdr(b, 6, &h), -1002); /* RSV */
}

/* ---------- loopback ---------- */
static void test_ping_pong_and_text(void) {
    int fd = cl_upgrade();
    uint8_t b[256];
    send_frame(fd, 9, true, (const uint8_t *)"abc", 3);
    ssize_t n = cl_read(fd, b, sizeof b);
    CT_EQ(n, 5);
    CT_EQ(b[0], 0x8A); CT_EQ(b[1], 3); CT_MEM_EQ(b + 2, "abc", 3);
    g_texts = 0;
    send_frame(fd, 1, true, (const uint8_t *)"hello", 5);
    pump(4);
    CT_EQ(g_texts, 1); CT_EQ(g_len, 5); CT_MEM_EQ(g_text, "hello", 5);
    close(fd);
    pump(4);
}

static void test_split_frame(void) {
    int fd = cl_upgrade();
    size_t l = mkframe(big, 1, true, (const uint8_t *)"split-me", 8, true);
    g_texts = 0;
    for (size_t i = 0; i < l; i++) {
        send(fd, big + i, 1, 0);
        pump(1);
    }
    pump(3);
    CT_EQ(g_texts, 1); CT_EQ(g_len, 8); CT_MEM_EQ(g_text, "split-me", 8);
    close(fd);
    pump(3);
}

static void test_fragments(void) {
    int fd = cl_upgrade();
    g_texts = 0;
    send_frame(fd, 1, false, (const uint8_t *)"foo", 3);
    send_frame(fd, 9, true, (const uint8_t *)"p", 1); /* control frame interleaved */
    send_frame(fd, 0, true, (const uint8_t *)"bar", 3);
    pump(4);
    CT_EQ(g_texts, 1); CT_EQ(g_len, 6); CT_MEM_EQ(g_text, "foobar", 6);
    close(fd);
    pump(3);

    /* continuation total exceeds the cap */
    fd = cl_upgrade();
    static uint8_t chunk[HP_WS_MAX_FRAME];
    memset(chunk, 'a', sizeof chunk);
    g_texts = 0;
    send_frame(fd, 1, false, chunk, sizeof chunk);
    send_frame(fd, 0, true, (const uint8_t *)"x", 1);
    uint8_t b[64];
    ssize_t n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1009);
    CT_EQ(g_texts, 0);
    close(fd);

    /* continuation without a start */
    fd = cl_upgrade();
    send_frame(fd, 0, true, (const uint8_t *)"x", 1);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1002);
    close(fd);
    pump(3);
}

static void test_bad_frames(void) {
    uint8_t b[64];
    ssize_t n;
    /* oversized: header only, no payload sent, still rejected */
    int fd = cl_upgrade();
    uint8_t h[14] = {0x81, 0x80 | 127, 0, 0, 0, 0, 0, 1, 0, 1}; /* len 65537, mask zero */
    send(fd, h, 14, 0);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1009);
    CT_CHECK(peer_closed(fd));
    close(fd);

    /* 64-bit length with top bit set */
    fd = cl_upgrade();
    memset(h, 0, sizeof h);
    h[0] = 0x81; h[1] = 0x80 | 127; h[2] = 0xFF;
    send(fd, h, 14, 0);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1009);
    close(fd);

    /* unmasked */
    fd = cl_upgrade();
    size_t l = mkframe(big, 1, true, (const uint8_t *)"hi", 2, false);
    send(fd, big, l, 0);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1002);
    close(fd);

    /* invalid UTF-8 */
    fd = cl_upgrade();
    g_texts = 0;
    send_frame(fd, 1, true, (const uint8_t *)"\xc3", 1);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1007);
    CT_EQ(g_texts, 0);
    close(fd);

    /* binary frame */
    fd = cl_upgrade();
    send_frame(fd, 2, true, (const uint8_t *)"x", 1);
    n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1003);
    close(fd);
    pump(3);
}

static void test_close_echo_and_midframe_disconnect(void) {
    int fd = cl_upgrade();
    uint8_t b[64], cp[2] = {0x03, 0xE8};
    send_frame(fd, 8, true, cp, 2);
    ssize_t n = cl_read(fd, b, sizeof b);
    CT_EQ(close_code(b, n), 1000);
    CT_CHECK(peer_closed(fd));
    close(fd);

    int before = g_closes;
    fd = cl_upgrade();
    size_t l = mkframe(big, 1, true, (const uint8_t *)"truncated", 9, true);
    send(fd, big, l - 4, 0);
    pump(3);
    close(fd);
    pump(4);
    CT_EQ(g_closes, before + 1);
}

static void test_http_garbage_and_slow_handshake(void) {
    uint8_t b[256];
    int fd = cl_connect();
    send(fd, "POST / HTTP/1.1\r\n\r\n", 19, 0);
    ssize_t n = cl_read(fd, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    CT_CHECK(n > 0 && strstr((char *)b, "400"));
    close(fd);

    fd = cl_connect();
    send(fd, "\x16\x03\x01\x02\x00garbage", 12, 0);
    n = cl_read(fd, b, sizeof b - 1);
    CT_CHECK(n > 0);
    close(fd);

    /* a stalled half-handshake must not block another client */
    int slow = cl_connect();
    send(slow, "GET / HT", 8, 0);
    pump(2);
    int ok = cl_upgrade();
    send_frame(ok, 9, true, NULL, 0);
    n = cl_read(ok, b, sizeof b);
    CT_EQ(n, 2);
    close(ok);
    close(slow);
    pump(3);
}

static void test_send_max_frame(void) {
    int fd = cl_upgrade();
    send_frame(fd, 1, true, (const uint8_t *)"x", 1);
    pump(4);
    static char payload[HP_WS_MAX_FRAME];
    memset(payload, 'q', sizeof payload);
    CT_EQ(hp_ws_send(g_conn, payload, sizeof payload), 0);
    CT_EQ(hp_ws_send(g_conn, payload, sizeof payload + 1), -2);
    static uint8_t rx[HP_WS_MAX_FRAME + 64];
    size_t tot = 0;
    for (int i = 0; i < 200 && tot < 10 + HP_WS_MAX_FRAME; i++) {
        pump(1);
        ssize_t n = recv(fd, rx + tot, sizeof rx - tot, MSG_DONTWAIT);
        if (n > 0) tot += (size_t)n;
    }
    CT_EQ(tot, 10 + HP_WS_MAX_FRAME);
    CT_EQ(rx[0], 0x81); CT_EQ(rx[1], 127);
    CT_EQ(rx[7], 0x01); CT_EQ(rx[8], 0x00); CT_EQ(rx[9], 0x00); /* 65536 as 8 bytes big-endian */
    CT_CHECK(rx[2] == 0 && rx[3] == 0 && rx[4] == 0 && rx[5] == 0 && rx[6] == 0);
    CT_MEM_EQ(rx + 10, payload, HP_WS_MAX_FRAME);
    close(fd);
    pump(3);
}

static void test_sigpipe_safe(void) {
    int fd = cl_upgrade();
    send_frame(fd, 9, true, (const uint8_t *)"x", 1);
    close(fd); /* peer gone before the pong is written */
    pump(6);
    CT_CHECK(1); /* reaching here means SIGPIPE did not kill us */
}

static int g_so[16][3], g_nso;
static int rec_sso(int fd, int level, int opt, const void *val, socklen_t len) {
    (void)fd; (void)len;
    if (g_nso < 16) { g_so[g_nso][0] = level; g_so[g_nso][1] = opt; g_so[g_nso][2] = *(const int *)val; g_nso++; }
    return 0;
}

static bool so_has(int level, int opt, int val) {
    for (int i = 0; i < g_nso; i++)
        if (g_so[i][0] == level && g_so[i][1] == opt && g_so[i][2] == val) return true;
    return false;
}

static void test_keepalive_on_accept(void) {
    g_nso = 0;
    hp_ws_set_setsockopt(rec_sso);
    int fd = cl_connect();
    pump(3);
    CT_CHECK(so_has(SOL_SOCKET, SO_KEEPALIVE, 1));
#ifdef TCP_KEEPIDLE
    CT_CHECK(so_has(IPPROTO_TCP, TCP_KEEPIDLE, 60));
#endif
#ifdef TCP_KEEPINTVL
    CT_CHECK(so_has(IPPROTO_TCP, TCP_KEEPINTVL, 10));
#endif
#ifdef TCP_KEEPCNT
    CT_CHECK(so_has(IPPROTO_TCP, TCP_KEEPCNT, 3));
#endif
    hp_ws_set_setsockopt(NULL);
    close(fd);
    pump(3);
}

static void run_for_ms(int ms) {
    for (int i = 0; i < ms / 5; i++) hp_ws_poll(srv, 5);
}

static void test_no_idle_close_when_authed_but_auth_and_handshake_deadlines(void) {
    CT_EQ(hp_ws_set_timeouts(srv, 150, 100), 0); /* idle shorter than the test's silence */
    hp_ws_set_handshake_ms(srv, 150);
    /* authenticated: silent well past idle_ms, still open and answering */
    int fd = cl_upgrade();
    send_frame(fd, 1, true, (const uint8_t *)"x", 1);
    pump(4);
    hp_ws_conn_set_authed(g_conn);
    int closes = g_closes;
    run_for_ms(500);
    CT_EQ(g_closes, closes);
    send_frame(fd, 9, true, NULL, 0); /* an app-level PING from an old client is still answered */
    uint8_t b[16];
    CT_EQ(cl_read(fd, b, sizeof b), 2);
    CT_EQ(b[0], 0x8a);
    close(fd);
    pump(3);
    /* upgraded but never authenticated: closed by the auth deadline (1008) */
    hp_ws_set_timeouts(srv, 150, 60000);
    fd = cl_upgrade();
    run_for_ms(400);
    uint8_t cl[16];
    ssize_t n = cl_read(fd, cl, sizeof cl);
    CT_EQ(close_code(cl, n), 1008);
    close(fd);
    /* TCP connected but no handshake: dropped by the handshake deadline */
    fd = cl_connect();
    pump(2);
    closes = g_closes;
    run_for_ms(400);
    CT_EQ(cl_read(fd, cl, sizeof cl), 0); /* EOF */
    close(fd);
    hp_ws_set_handshake_ms(srv, HP_WS_HANDSHAKE_MS);
    hp_ws_set_timeouts(srv, HP_WS_AUTH_MS, HP_WS_IDLE_MS);
}

int main(void) {
    hp_ws_cb_t cb = {on_text, on_close};
    srv = hp_ws_listen(0, "tok", &cb, NULL);
    CT_CHECK(srv != NULL);
    if (!srv) return 1;
    CT_RUN(test_helpers);
    CT_RUN(test_handshake_parse);
    CT_RUN(test_frame_hdr);
    CT_RUN(test_ping_pong_and_text);
    CT_RUN(test_split_frame);
    CT_RUN(test_fragments);
    CT_RUN(test_bad_frames);
    CT_RUN(test_close_echo_and_midframe_disconnect);
    CT_RUN(test_http_garbage_and_slow_handshake);
    CT_RUN(test_send_max_frame);
    CT_RUN(test_sigpipe_safe);
    CT_RUN(test_keepalive_on_accept);
    CT_RUN(test_no_idle_close_when_authed_but_auth_and_handshake_deadlines);
    hp_ws_close(srv);
    return CT_DONE();
}
