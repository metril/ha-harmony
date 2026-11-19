#define _POSIX_C_SOURCE 200809L
#include "hub/net/ws.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "hub/hp_errors.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HS_MAX 8192
#define RBUF_CAP (HP_WS_MAX_FRAME + 16)

/* ------------------------------------------------------------------ SHA1 */

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80], a, b, c, d, e;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void hp_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rem = len - i;
    if (rem) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tl = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24);
        out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8);
        out[4 * k + 3] = (uint8_t)h[k];
    }
}

/* ---------------------------------------------------------------- base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t hp_base64_encode(const uint8_t *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

size_t hp_base64_decode(const char *s, size_t len, uint8_t *out, size_t cap) {
    if (!hp_base64_valid(s, len)) return (size_t)-1;
    size_t pad = len && s[len - 1] == '=' ? (len > 1 && s[len - 2] == '=' ? 2 : 1) : 0;
    size_t n = len / 4 * 3 - pad;
    if (n > cap) return (size_t)-1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i + (size_t)k];
            uint32_t d = c >= 'A' && c <= 'Z' ? (uint32_t)(c - 'A')
                       : c >= 'a' && c <= 'z' ? (uint32_t)(c - 'a' + 26)
                       : c >= '0' && c <= '9' ? (uint32_t)(c - '0' + 52)
                       : c == '+' ? 62u : c == '/' ? 63u : 0u;
            v = (v << 6) | d;
        }
        for (int k = 0; k < 3 && o < n; k++) out[o++] = (uint8_t)(v >> (16 - 8 * k));
    }
    return n;
}

bool hp_base64_valid(const char *s, size_t len) {
    if (len % 4) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/';
        if (ok) continue;
        if (c == '=' && i >= len - 2 && (i == len - 1 || s[len - 1] == '=')) continue;
        return false;
    }
    return true;
}

void hp_ws_accept_key(const char *key, size_t key_len, char out[29]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t buf[64 + sizeof GUID], dig[20];
    if (key_len > 64) key_len = 64;
    memcpy(buf, key, key_len);
    memcpy(buf + key_len, GUID, sizeof GUID - 1);
    hp_sha1(buf, key_len + sizeof GUID - 1, dig);
    hp_base64_encode(dig, 20, out);
}

/* ---------------------------------------------------------------- UTF-8 */

bool hp_utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += need + 1;
    }
    return true;
}

/* ------------------------------------------------------------- handshake */

static bool ieq(const char *a, size_t al, const char *lit) {
    size_t n = strlen(lit);
    if (al != n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lit[i]) return false;
    }
    return true;
}

static bool has_token(const char *v, size_t len, const char *tok) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < len && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (i > s && ieq(v + s, i - s, tok)) return true;
    }
    return false;
}

int hp_ws_parse_handshake(const char *buf, size_t len, size_t *consumed, char accept[29]) {
    static const char GET[] = "GET ";
    size_t pre = len < 4 ? len : 4;
    if (memcmp(buf, GET, pre) != 0) return -400;
    size_t end = 0;
    for (size_t i = 0; i + 3 < len && i + 4 <= HS_MAX; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = i + 4;
            break;
        }
    }
    if (!end) return len >= HS_MAX ? -431 : HP_WS_HS_NEED_MORE;
    if (memchr(buf, '\0', end)) return -400;

    /* request line */
    size_t pos = 0;
    const char *eol = memchr(buf, '\r', end);
    size_t ll = (size_t)(eol - buf);
    if (ll < 14 || memcmp(buf + ll - 9, " HTTP/1.1", 9) != 0) return -400;
    pos = ll + 2;

    bool up = false, conn = false, ver = false;
    const char *key = NULL;
    size_t keylen = 0;
    while (pos + 2 < end) {
        const char *ls = buf + pos;
        const char *le = memchr(ls, '\r', end - pos);
        size_t l = (size_t)(le - ls);
        const char *colon = memchr(ls, ':', l);
        if (!colon || colon == ls) return -400;
        size_t nl = (size_t)(colon - ls);
        const char *v = colon + 1;
        size_t vl = l - nl - 1;
        while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
        if (ieq(ls, nl, "upgrade")) up = ieq(v, vl, "websocket");
        else if (ieq(ls, nl, "connection")) conn = conn || has_token(v, vl, "upgrade");
        else if (ieq(ls, nl, "sec-websocket-version")) ver = ieq(v, vl, "13");
        else if (ieq(ls, nl, "sec-websocket-key")) {
            if (key) return -400;
            key = v;
            keylen = vl;
        }
        pos += l + 2;
    }
    if (!up || !conn || !key || keylen != 24 || !hp_base64_valid(key, keylen) ||
        key[22] != '=' || key[23] != '=')
        return -400;
    if (!ver) return -426;
    hp_ws_accept_key(key, keylen, accept);
    *consumed = end;
    return HP_WS_HS_OK;
}

/* ---------------------------------------------------------------- frames */

int hp_ws_parse_frame_hdr(const uint8_t *b, size_t len, hp_ws_frame_hdr_t *h) {
    if (len < 2) return HP_WS_FRAME_NEED_MORE;
    if (b[0] & 0x70) return -1002;
    h->fin = (b[0] & 0x80) != 0;
    h->opcode = b[0] & 0x0F;
    if (!(b[1] & 0x80)) return -1002;
    if ((h->opcode >= 3 && h->opcode <= 7) || h->opcode > 10) return -1002;
    uint64_t pl = b[1] & 0x7F;
    size_t off = 2;
    if (pl == 126) {
        if (len < 4) return HP_WS_FRAME_NEED_MORE;
        pl = ((uint64_t)b[2] << 8) | b[3];
        off = 4;
    } else if (pl == 127) {
        if (len < 10) return HP_WS_FRAME_NEED_MORE;
        pl = 0;
        for (int i = 0; i < 8; i++) pl = (pl << 8) | b[2 + i];
        off = 10;
    }
    if (h->opcode >= 8 && (!h->fin || pl > 125)) return -1002;
    if (pl > HP_WS_MAX_FRAME) return -1009;
    if (len < off + 4) return HP_WS_FRAME_NEED_MORE;
    memcpy(h->mask, b + off, 4);
    h->payload_len = pl;
    h->hdr_len = off + 4;
    return HP_WS_FRAME_OK;
}

/* ---------------------------------------------------------------- server */

enum { ST_FREE = 0, ST_HANDSHAKE, ST_OPEN, ST_CLOSING };

struct hp_ws_conn {
    hp_ws_server_t *srv;
    int fd;
    int state;
    bool upgraded;
    bool dead;
    bool in_frag;
    uint8_t *rbuf;
    size_t rlen;
    uint8_t *msg;
    size_t mlen;
    uint8_t *out;
    size_t ocap, olen, ooff;
    int64_t deadline; /* 0 = none: handshake / close flush */
    int64_t auth_dl;  /* 0 = none: hello must complete by then (open conns) */
    int64_t idle_dl;  /* 0 = none: next inbound frame must arrive by then */
    void *user;
};

struct hp_ws_server {
    int lfd;
    uint16_t port;
    char *token;
    hp_ws_cb_t cb;
    void *user;
    int auth_ms, idle_ms;
    int (*extra_fds)(void *arg, int out[8]);
    void *extra_arg;
    hp_ws_conn_t conns[HP_WS_MAX_CONNS];
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nb(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

hp_ws_server_t *hp_ws_listen(uint16_t port, const char *token, const hp_ws_cb_t *cb, void *user) {
    if (!cb || !cb->on_text) return NULL;
    signal(SIGPIPE, SIG_IGN);
    hp_ws_server_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cb = *cb;
    s->user = user;
    s->auth_ms = HP_WS_AUTH_MS;
    s->idle_ms = HP_WS_IDLE_MS;
    s->token = token ? strdup(token) : NULL;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) goto fail;
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(s->lfd, 16) < 0) goto fail;
    socklen_t al = sizeof a;
    if (getsockname(s->lfd, (struct sockaddr *)&a, &al) < 0) goto fail;
    s->port = ntohs(a.sin_port);
    set_nb(s->lfd);
    for (int i = 0; i < HP_WS_MAX_CONNS; i++) {
        s->conns[i].srv = s;
        s->conns[i].fd = -1;
    }
    return s;
fail:
    if (s->lfd >= 0) close(s->lfd);
    free(s->token);
    free(s);
    return NULL;
}

uint16_t hp_ws_port(const hp_ws_server_t *s) { return s->port; }
const char *hp_ws_token(const hp_ws_server_t *s) { return s->token; }
int hp_ws_set_timeouts(hp_ws_server_t *s, int auth_ms, int idle_ms) {
    if (!s || auth_ms <= 0 || idle_ms <= 0) return -HP_E_INVAL;
    s->auth_ms = auth_ms;
    s->idle_ms = idle_ms;
    return 0;
}
void hp_ws_conn_set_authed(hp_ws_conn_t *c) { c->auth_dl = 0; }
void hp_ws_foreach(hp_ws_server_t *s, void (*fn)(hp_ws_conn_t *, void *), void *arg) {
    for (int i = 0; i < HP_WS_MAX_CONNS; i++)
        if (s->conns[i].state == ST_OPEN && s->conns[i].upgraded && !s->conns[i].dead) fn(&s->conns[i], arg);
}
void hp_ws_conn_set_user(hp_ws_conn_t *c, void *u) { c->user = u; }
void *hp_ws_conn_user(const hp_ws_conn_t *c) { return c->user; }

static void conn_free(hp_ws_conn_t *c) {
    hp_ws_server_t *s = c->srv;
    if (c->upgraded && s->cb.on_close) s->cb.on_close(c, s->user);
    if (c->fd >= 0) close(c->fd);
    free(c->rbuf);
    free(c->msg);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->srv = s;
    c->fd = -1;
}

static int out_append(hp_ws_conn_t *c, const uint8_t *p, size_t n) {
    if (c->olen - c->ooff + n > HP_WS_MAX_OUT) {
        c->dead = true;
        return -HP_E_BUSY;
    }
    if (c->ooff == c->olen) c->ooff = c->olen = 0;
    if (c->olen + n > c->ocap) {
        size_t nc = c->ocap ? c->ocap : 1024;
        while (nc < c->olen + n) nc *= 2;
        uint8_t *nb = realloc(c->out, nc);
        if (!nb) {
            c->dead = true;
            return -HP_E_BUSY;
        }
        c->out = nb;
        c->ocap = nc;
    }
    memcpy(c->out + c->olen, p, n);
    c->olen += n;
    return 0;
}

static void flush_out(hp_ws_conn_t *c) {
    while (c->ooff < c->olen) {
        ssize_t n = send(c->fd, c->out + c->ooff, c->olen - c->ooff, MSG_NOSIGNAL);
        if (n > 0) {
            c->ooff += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            c->dead = true;
            return;
        }
    }
    if (c->state == ST_CLOSING) c->dead = true;
}

static int send_frame(hp_ws_conn_t *c, uint8_t opcode, const uint8_t *p, size_t n) {
    if (c->state != ST_OPEN) return -HP_E_BUSY;
    uint8_t hdr[10];
    size_t hl = 2;
    hdr[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) {
        hdr[1] = (uint8_t)n;
    } else if (n <= 0xFFFF) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)n;
        hl = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i)));
        hl = 10;
    }
    int r = out_append(c, hdr, hl);
    if (!r && n) r = out_append(c, p, n);
    if (!r) flush_out(c);
    return r;
}

int hp_ws_send(hp_ws_conn_t *c, const char *text, size_t len) {
    if (len > HP_WS_MAX_FRAME) return -HP_E_TOOBIG;
    return send_frame(c, 1, (const uint8_t *)text, len);
}

void hp_ws_conn_close(hp_ws_conn_t *c, uint16_t code) {
    if (c->state == ST_OPEN) {
        uint8_t pl[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        send_frame(c, 8, pl, 2);
    }
    if (c->state != ST_FREE && !c->dead) {
        c->state = ST_CLOSING;
        c->deadline = now_ms() + HP_WS_CLOSE_MS;
        flush_out(c);
    }
}

static void http_fail(hp_ws_conn_t *c, int code) {
    const char *r = code == -426
        ? "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
        : code == -431
        ? "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
        : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    c->state = ST_CLOSING;
    c->deadline = now_ms() + HP_WS_CLOSE_MS;
    out_append(c, (const uint8_t *)r, strlen(r));
    flush_out(c);
}

static void deliver(hp_ws_conn_t *c, const uint8_t *p, size_t n) {
    if (!hp_utf8_valid(p, n)) {
        hp_ws_conn_close(c, 1007);
        return;
    }
    c->srv->cb.on_text(c, (const char *)p, n, c->srv->user);
}

static void handle_frame(hp_ws_conn_t *c, const hp_ws_frame_hdr_t *h, uint8_t *p) {
    size_t n = (size_t)h->payload_len;
    c->idle_dl = now_ms() + c->srv->idle_ms; /* any inbound frame counts */
    for (size_t i = 0; i < n; i++) p[i] ^= h->mask[i & 3];
    switch (h->opcode) {
    case 0:
        if (!c->in_frag) { hp_ws_conn_close(c, 1002); return; }
        if (c->mlen + n > HP_WS_MAX_FRAME) { hp_ws_conn_close(c, 1009); return; }
        memcpy(c->msg + c->mlen, p, n);
        c->mlen += n;
        if (h->fin) {
            c->in_frag = false;
            size_t m = c->mlen;
            c->mlen = 0;
            deliver(c, c->msg, m);
        }
        break;
    case 1:
        if (c->in_frag) { hp_ws_conn_close(c, 1002); return; }
        if (h->fin) {
            deliver(c, p, n);
        } else {
            if (!c->msg && !(c->msg = malloc(HP_WS_MAX_FRAME))) { hp_ws_conn_close(c, 1011); return; }
            memcpy(c->msg, p, n);
            c->mlen = n;
            c->in_frag = true;
        }
        break;
    case 2:
        hp_ws_conn_close(c, 1003);
        break;
    case 8: {
        if (n == 1) { hp_ws_conn_close(c, 1002); return; }
        uint16_t code = n >= 2 ? (uint16_t)((p[0] << 8) | p[1]) : 1000;
        bool bad = n >= 2 && (code < 1000 || code == 1004 || code == 1005 || code == 1006 ||
                              (code > 1011 && code < 3000) || code >= 5000);
        hp_ws_conn_close(c, bad ? 1002 : code);
        break;
    }
    case 9:
        send_frame(c, 10, p, n);
        break;
    default: /* 10: pong, ignored */
        break;
    }
}

static void process_input(hp_ws_conn_t *c) {
    while (!c->dead && c->rlen) {
        if (c->state == ST_HANDSHAKE) {
            size_t used = 0;
            char acc[29];
            int r = hp_ws_parse_handshake((const char *)c->rbuf, c->rlen, &used, acc);
            if (r == HP_WS_HS_NEED_MORE) return;
            if (r < 0) { http_fail(c, r); return; }
            char resp[256];
            int rl = snprintf(resp, sizeof resp,
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
            c->state = ST_OPEN;
            c->upgraded = true;
            c->deadline = 0;
            c->auth_dl = now_ms() + c->srv->auth_ms;
            c->idle_dl = now_ms() + c->srv->idle_ms;
            out_append(c, (const uint8_t *)resp, (size_t)rl);
            flush_out(c);
            memmove(c->rbuf, c->rbuf + used, c->rlen - used);
            c->rlen -= used;
        } else if (c->state == ST_OPEN) {
            hp_ws_frame_hdr_t h;
            int r = hp_ws_parse_frame_hdr(c->rbuf, c->rlen, &h);
            if (r == HP_WS_FRAME_NEED_MORE) return;
            if (r < 0) { hp_ws_conn_close(c, (uint16_t)-r); return; }
            size_t total = h.hdr_len + (size_t)h.payload_len;
            if (c->rlen < total) return;
            handle_frame(c, &h, c->rbuf + h.hdr_len);
            memmove(c->rbuf, c->rbuf + total, c->rlen - total);
            c->rlen -= total;
        } else {
            c->rlen = 0; /* closing: ignore further input */
        }
    }
}

static void conn_readable(hp_ws_conn_t *c) {
    for (;;) {
        size_t room = RBUF_CAP - c->rlen;
        if (!room) return; /* full buffer is always a full frame; processed below */
        ssize_t n = recv(c->fd, c->rbuf + c->rlen, room, 0);
        if (n > 0) {
            c->rlen += (size_t)n;
            process_input(c);
            if (c->dead || c->state == ST_CLOSING) return;
        } else if (n == 0) {
            c->dead = true;
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            c->dead = true;
            return;
        }
    }
}

static void accept_one(hp_ws_server_t *s) {
    int fd = accept(s->lfd, NULL, NULL);
    if (fd < 0) return;
    hp_ws_conn_t *c = NULL;
    for (int i = 0; i < HP_WS_MAX_CONNS; i++)
        if (s->conns[i].state == ST_FREE) { c = &s->conns[i]; break; }
    uint8_t *rb = c ? malloc(RBUF_CAP) : NULL;
    if (!rb) {
        close(fd);
        return;
    }
    set_nb(fd);
    c->fd = fd;
    c->rbuf = rb;
    c->state = ST_HANDSHAKE;
    c->deadline = now_ms() + HP_WS_HANDSHAKE_MS;
}

void hp_ws_set_extra_fds(hp_ws_server_t *s, int (*fn)(void *arg, int out[8]), void *arg) {
    if (!s) return;
    s->extra_fds = fn;
    s->extra_arg = arg;
}

int hp_ws_poll(hp_ws_server_t *s, int timeout_ms) {
    if (!s) return -HP_E_INVAL;
    struct pollfd fds[9 + HP_WS_MAX_CONNS];
    int map[9 + HP_WS_MAX_CONNS];
    int nf = 0;
    int64_t now = now_ms();
    int64_t wait = timeout_ms < 0 ? INT32_MAX : timeout_ms;
    fds[nf].fd = s->lfd; fds[nf].events = POLLIN; fds[nf].revents = 0; map[nf++] = -1;
    for (int i = 0; i < HP_WS_MAX_CONNS; i++) {
        hp_ws_conn_t *c = &s->conns[i];
        if (c->state == ST_FREE) continue;
        fds[nf].fd = c->fd;
        fds[nf].events = (c->state == ST_CLOSING ? 0 : POLLIN) | (c->ooff < c->olen ? POLLOUT : 0);
        fds[nf].revents = 0;
        map[nf++] = i;
        if (c->deadline) {
            int64_t d = c->deadline - now;
            if (d < 0) d = 0;
            if (d < wait) wait = d;
        }
        if (c->state == ST_OPEN) {
            int64_t dl[2] = {c->auth_dl, c->idle_dl};
            for (int k = 0; k < 2; k++) {
                if (!dl[k]) continue;
                int64_t d = dl[k] - now;
                if (d < 0) d = 0;
                if (d < wait) wait = d;
            }
        }
    }
    if (s->extra_fds) { /* wake-only: the owner services these fds itself */
        int ef[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int ne = s->extra_fds(s->extra_arg, ef);
        for (int k = 0; k < ne && k < 8; k++) {
            fds[nf].fd = ef[k]; fds[nf].events = POLLIN; fds[nf].revents = 0; map[nf++] = -2;
        }
    }
    int r = poll(fds, (nfds_t)nf, (int)wait);
    if (r < 0) return errno == EINTR ? 0 : -HP_E_INVAL;
    for (int k = 0; k < nf; k++) {
        if (!fds[k].revents) continue;
        if (map[k] == -2) continue;
        if (map[k] < 0) {
            accept_one(s);
            continue;
        }
        hp_ws_conn_t *c = &s->conns[map[k]];
        if (fds[k].revents & POLLOUT) flush_out(c);
        if (!c->dead && (fds[k].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (c->state == ST_CLOSING) {
                if (fds[k].revents & (POLLHUP | POLLERR)) c->dead = true;
            } else {
                conn_readable(c);
            }
        }
    }
    now = now_ms();
    for (int i = 0; i < HP_WS_MAX_CONNS; i++) {
        hp_ws_conn_t *c = &s->conns[i];
        if (c->state == ST_FREE) continue;
        if (c->deadline && now >= c->deadline) c->dead = true;
        if (c->state == ST_OPEN && !c->dead) {
            if (c->auth_dl && now >= c->auth_dl) hp_ws_conn_close(c, 1008);
            else if (c->idle_dl && now >= c->idle_dl) hp_ws_conn_close(c, 1001);
        }
        if (c->dead) conn_free(c);
    }
    return r;
}

void hp_ws_close(hp_ws_server_t *s) {
    if (!s) return;
    for (int i = 0; i < HP_WS_MAX_CONNS; i++)
        if (s->conns[i].state != ST_FREE) conn_free(&s->conns[i]);
    close(s->lfd);
    free(s->token);
    free(s);
}
