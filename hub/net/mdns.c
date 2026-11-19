#include "hub/net/mdns.h"

#include <stdio.h>
#include <string.h>

#include "hub/hp_errors.h"

#define SVC_NAME "_haharmony._tcp.local."
#define ENUM_NAME "_services._dns-sd._udp.local."

enum { T_A = 1, T_PTR = 12, T_TXT = 16, T_SRV = 33, T_ANY = 255 };
#define CLASS_IN 1
#define CLASS_ANY 255
#define FLUSH 0x8000u
#define MAX_JUMPS 16

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- name decoding (hostile input safe) ---- */

/* Decodes the name at *off into dotted lowercase text (trailing '.'; "." for
 * root) and advances *off past it in the original position (not following
 * pointers). Pointers must point strictly backwards and at most MAX_JUMPS are
 * followed. 0 ok, -1 malformed. */
static int name_decode_ex(const uint8_t *p, size_t len, size_t *off, char *out, size_t cap, int fold) {
    size_t pos = *off, o = 0, end = 0;
    int jumps = 0;
    for (;;) {
        if (pos >= len) return -1;
        uint8_t b = p[pos];
        if (b == 0) {
            if (!end) end = pos + 1;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return -1;
            size_t tgt = (size_t)(((b & 0x3F) << 8) | p[pos + 1]);
            if (!end) end = pos + 2;
            if (tgt >= pos || ++jumps > MAX_JUMPS) return -1; /* backwards only: no loops */
            pos = tgt;
            continue;
        }
        if (b & 0xC0) return -1; /* 01/10 label types are not valid */
        if (pos + 1 + b > len) return -1;
        if (o + b + 2 > cap) return -1; /* name too long (label + dot + NUL) */
        for (unsigned i = 0; i < b; i++) {
            uint8_t c = p[pos + 1 + i];
            out[o++] = (char)((fold && c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        out[o++] = '.';
        pos += 1u + b;
    }
    if (o == 0) out[o++] = '.';
    out[o] = 0;
    *off = end;
    return 0;
}

static int name_decode(const uint8_t *p, size_t len, size_t *off, char *out, size_t cap) {
    return name_decode_ex(p, len, off, out, cap, 1);
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

int hp_mdns_svc_init(hp_mdns_svc_t *s, const char *hub_id, uint16_t port, uint32_t ipv4_host, unsigned proto_v) {
    if (!hub_id) return -HP_E_INVAL;
    size_t n = strlen(hub_id);
    if (n == 0 || n > HP_MDNS_LABEL_MAX) return -HP_E_INVAL;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)hub_id[i] < 0x21 || (unsigned char)hub_id[i] > 0x7e || hub_id[i] == '.')
            return -HP_E_INVAL;
    memset(s, 0, sizeof *s);
    memcpy(s->hub_id, hub_id, n);
    snprintf(s->inst, sizeof s->inst, "%s." SVC_NAME, hub_id);
    snprintf(s->host, sizeof s->host, "%s.local.", hub_id);
    s->port = port;
    s->ipv4 = ipv4_host;
    s->proto_v = proto_v;
    return 0;
}

/* ---- query parsing ---- */

int hp_mdns_parse_query(const hp_mdns_svc_t *s, const uint8_t *p, size_t len, hp_mdns_query_t *q) {
    memset(q, 0, sizeof *q);
    if (!p || len < 12) return -HP_E_INVAL;
    uint16_t flags = rd16(p + 2);
    if (flags & 0x8000) return -HP_E_INVAL;             /* a response */
    if (((flags >> 11) & 0xF) != 0) return -HP_E_INVAL; /* opcode != QUERY */
    q->id = rd16(p);
    unsigned qd = rd16(p + 4), an = rd16(p + 6);
    size_t off = 12;
    char name[HP_MDNS_NAME_MAX + 1];
    unsigned want = 0;
    for (unsigned i = 0; i < qd; i++) {
        size_t qoff = off;
        if (name_decode(p, len, &off, name, sizeof name) < 0) return -HP_E_INVAL;
        if (off + 4 > len) return -HP_E_INVAL;
        unsigned type = rd16(p + off), cls = rd16(p + off + 2);
        off += 4;
        unsigned c = cls & 0x7FFF;
        if (c != CLASS_IN && c != CLASS_ANY) continue;
        unsigned m = 0;
        if (ieq(name, SVC_NAME) && (type == T_PTR || type == T_ANY)) m |= HP_MDNS_PTR;
        if (ieq(name, ENUM_NAME) && (type == T_PTR || type == T_ANY)) m |= HP_MDNS_ENUM;
        if (ieq(name, s->inst)) {
            if (type == T_SRV || type == T_ANY) m |= HP_MDNS_SRV;
            if (type == T_TXT || type == T_ANY) m |= HP_MDNS_TXT;
        }
        if (ieq(name, s->host) && (type == T_A || type == T_ANY)) m |= HP_MDNS_A;
        if (m && (cls & 0x8000)) q->unicast = true;
        if (m && !q->have_q) { /* keep the original case for the legacy echo */
            size_t o2 = qoff;
            if (name_decode_ex(p, len, &o2, q->qname, sizeof q->qname, 0) == 0) {
                q->have_q = true;
                q->qtype = (uint16_t)type;
                q->qclass = (uint16_t)c;
            }
        }
        want |= m;
    }
    /* Known-answer suppression (PTR, enum PTR, A only). A truncated answer
     * section just ends the scan: the query itself was valid. */
    for (unsigned i = 0; i < an && want; i++) {
        if (name_decode(p, len, &off, name, sizeof name) < 0 || off + 10 > len) break;
        unsigned type = rd16(p + off);
        uint32_t ttl = rd32(p + off + 4);
        size_t rdlen = rd16(p + off + 8);
        off += 10;
        if (off + rdlen > len) break;
        if (type == T_PTR) {
            char rd[HP_MDNS_NAME_MAX + 1];
            size_t ro = off;
            if (name_decode(p, len, &ro, rd, sizeof rd) == 0 && ttl >= HP_MDNS_TTL_SERVICE / 2) {
                if (ieq(name, SVC_NAME) && ieq(rd, s->inst)) want &= ~(unsigned)HP_MDNS_PTR;
                if (ieq(name, ENUM_NAME) && ieq(rd, SVC_NAME)) want &= ~(unsigned)HP_MDNS_ENUM;
            }
        } else if (type == T_A && rdlen == 4 && ieq(name, s->host) && rd32(p + off) == s->ipv4 &&
                   ttl >= HP_MDNS_TTL_HOST / 2) {
            want &= ~(unsigned)HP_MDNS_A;
        }
        off += rdlen;
    }
    q->want = want;
    return 0;
}

/* ---- response building ---- */

typedef struct {
    uint8_t *b;
    size_t cap, n;
    int err;
} wr_t;

static void put(wr_t *w, const void *d, size_t n) {
    if (w->err || w->n + n > w->cap) { w->err = 1; return; }
    memcpy(w->b + w->n, d, n);
    w->n += n;
}
static void put8(wr_t *w, unsigned v) { uint8_t b = (uint8_t)v; put(w, &b, 1); }
static void put16(wr_t *w, unsigned v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; put(w, b, 2); }
static void put32(wr_t *w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(w, b, 4);
}
static void put_name(wr_t *w, const char *name) {
    while (*name) {
        const char *e = strchr(name, '.');
        size_t l = e ? (size_t)(e - name) : strlen(name);
        if (l == 0 || l > HP_MDNS_LABEL_MAX) { w->err = 1; return; }
        put8(w, (unsigned)l);
        put(w, name, l);
        name += l;
        if (*name == '.') name++;
    }
    put8(w, 0);
}

/* Writes the RR header and returns the offset of the rdlength field. */
static size_t rr_head(wr_t *w, const char *name, unsigned type, bool flush, uint32_t ttl) {
    put_name(w, name);
    put16(w, type);
    put16(w, CLASS_IN | (flush ? FLUSH : 0));
    put32(w, ttl);
    size_t at = w->n;
    put16(w, 0);
    return at;
}
static void rr_end(wr_t *w, size_t at) {
    if (w->err) return;
    size_t rl = w->n - at - 2;
    w->b[at] = (uint8_t)(rl >> 8);
    w->b[at + 1] = (uint8_t)rl;
}

static void emit(wr_t *w, const hp_mdns_svc_t *s, unsigned rec, unsigned flags) {
    bool bye = flags & HP_MDNS_F_GOODBYE, legacy = flags & HP_MDNS_F_LEGACY;
    uint32_t svc_ttl = bye ? 0 : HP_MDNS_TTL_SERVICE, host_ttl = bye ? 0 : HP_MDNS_TTL_HOST;
    if (legacy && !bye) {
        if (svc_ttl > HP_MDNS_TTL_LEGACY) svc_ttl = HP_MDNS_TTL_LEGACY;
        if (host_ttl > HP_MDNS_TTL_LEGACY) host_ttl = HP_MDNS_TTL_LEGACY;
    }
    bool flush = !legacy;
    size_t at;
    switch (rec) {
    case HP_MDNS_PTR: /* shared record: never cache-flush */
        at = rr_head(w, SVC_NAME, T_PTR, false, svc_ttl);
        put_name(w, s->inst);
        rr_end(w, at);
        break;
    case HP_MDNS_ENUM:
        at = rr_head(w, ENUM_NAME, T_PTR, false, svc_ttl);
        put_name(w, SVC_NAME);
        rr_end(w, at);
        break;
    case HP_MDNS_SRV:
        at = rr_head(w, s->inst, T_SRV, flush, host_ttl);
        put16(w, 0); /* priority */
        put16(w, 0); /* weight */
        put16(w, s->port);
        put_name(w, s->host);
        rr_end(w, at);
        break;
    case HP_MDNS_TXT: {
        char kv[96];
        at = rr_head(w, s->inst, T_TXT, flush, svc_ttl);
        int n = snprintf(kv, sizeof kv, "hub_id=%s", s->hub_id);
        if (n < 0 || n > 255 || (size_t)n >= sizeof kv) { w->err = 1; return; }
        put8(w, (unsigned)n);
        put(w, kv, (size_t)n);
        n = snprintf(kv, sizeof kv, "v=%u", s->proto_v);
        put8(w, (unsigned)n);
        put(w, kv, (size_t)n);
        if (s->hw[0]) {
            n = snprintf(kv, sizeof kv, "hw=%s", s->hw);
            if (n < 0 || (size_t)n >= sizeof kv) { w->err = 1; return; }
            put8(w, (unsigned)n);
            put(w, kv, (size_t)n);
        }
        rr_end(w, at);
        break;
    }
    case HP_MDNS_A:
        at = rr_head(w, s->host, T_A, flush, host_ttl);
        put32(w, s->ipv4);
        rr_end(w, at);
        break;
    default: break;
    }
}

static unsigned count5(unsigned v) {
    unsigned n = 0;
    for (unsigned b = 1; b <= HP_MDNS_A; b <<= 1) n += (v & b) != 0;
    return n;
}

int hp_mdns_build_response_q(const hp_mdns_svc_t *s, unsigned answers, unsigned flags, uint16_t id,
                             const hp_mdns_query_t *q, uint8_t *out, size_t cap) {
    answers &= HP_MDNS_ALL;
    unsigned extra = 0;
    if (answers & HP_MDNS_PTR) extra |= HP_MDNS_SRV | HP_MDNS_TXT | HP_MDNS_A;
    if (answers & HP_MDNS_SRV) extra |= HP_MDNS_A;
    extra &= ~answers;
    wr_t w = {out, cap, 0, 0};
    bool echo = (flags & HP_MDNS_F_LEGACY) && q && q->have_q;
    put16(&w, (flags & HP_MDNS_F_LEGACY) ? id : 0);
    put16(&w, 0x8400); /* QR | AA */
    put16(&w, echo ? 1 : 0);
    put16(&w, count5(answers));
    put16(&w, 0);
    put16(&w, count5(extra));
    if (echo) {
        put_name(&w, q->qname);
        put16(&w, q->qtype);
        put16(&w, q->qclass);
    }
    for (unsigned b = 1; b <= HP_MDNS_A; b <<= 1)
        if (answers & b) emit(&w, s, b, flags);
    for (unsigned b = 1; b <= HP_MDNS_A; b <<= 1)
        if (extra & b) emit(&w, s, b, flags);
    return w.err ? -HP_E_TOOBIG : (int)w.n;
}

int hp_mdns_build_response(const hp_mdns_svc_t *s, unsigned answers, unsigned flags, uint16_t id,
                           uint8_t *out, size_t cap) {
    return hp_mdns_build_response_q(s, answers, flags, id, NULL, out, cap);
}
