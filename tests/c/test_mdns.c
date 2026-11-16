#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/net/mdns.h"
#include "hub/net/mdns_srv.h"
#include "hub/engine/sched.h"

#define IP 0x0A010203u /* 10.1.2.3 */

static hp_mdns_svc_t S;

typedef struct { uint8_t b[1500]; size_t n; } pk_t;

static void p8(pk_t *p, unsigned v) { p->b[p->n++] = (uint8_t)v; }
static void p16(pk_t *p, unsigned v) { p8(p, v >> 8); p8(p, v); }
static void p32(pk_t *p, uint32_t v) { p16(p, v >> 16); p16(p, v & 0xFFFF); }
static void pname(pk_t *p, const char *n) {
    while (*n) {
        const char *e = strchr(n, '.');
        size_t l = e ? (size_t)(e - n) : strlen(n);
        p8(p, (unsigned)l);
        memcpy(p->b + p->n, n, l);
        p->n += l;
        n += l + (e ? 1 : 0);
    }
    p8(p, 0);
}
static void hdr(pk_t *p, unsigned flags, unsigned qd, unsigned an) {
    p->n = 0;
    p16(p, 0x1234); p16(p, flags); p16(p, qd); p16(p, an); p16(p, 0); p16(p, 0);
}
static void question(pk_t *p, const char *name, unsigned type, unsigned cls) {
    pname(p, name); p16(p, type); p16(p, cls);
}
static pk_t query1(const char *name, unsigned type, unsigned cls) {
    pk_t p; hdr(&p, 0, 1, 0); question(&p, name, type, cls); return p;
}
static unsigned want_of(const pk_t *p, bool *qu) {
    hp_mdns_query_t q;
    CT_EQ(hp_mdns_parse_query(&S, p->b, p->n, &q), 0);
    if (qu) *qu = q.unicast;
    return q.want;
}

static void test_questions(void) {
    pk_t p = query1("_haharmony._tcp.local", 12, 1);
    bool qu;
    CT_EQ(want_of(&p, &qu), HP_MDNS_PTR); CT_CHECK(!qu);
    p = query1("_HaHarmony._TCP.local", 12, 0x8001);
    CT_EQ(want_of(&p, &qu), HP_MDNS_PTR); CT_CHECK(qu);
    p = query1("_services._dns-sd._udp.local", 12, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_ENUM);
    p = query1("sim-hub._haharmony._tcp.local", 33, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_SRV);
    p = query1("sim-hub._haharmony._tcp.local", 16, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_TXT);
    p = query1("sim-hub._haharmony._tcp.local", 255, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_SRV | HP_MDNS_TXT);
    p = query1("sim-hub.local", 1, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_A);
    p = query1("sim-hub.local", 28, 1); /* AAAA: not served */
    CT_EQ(want_of(&p, NULL), 0);
    p = query1("other.local", 1, 1);
    CT_EQ(want_of(&p, NULL), 0);
    p = query1("_haharmony._tcp.local", 12, 3); /* class CH */
    CT_EQ(want_of(&p, NULL), 0);
    /* two questions combine; QU only counts for matching ones */
    hdr(&p, 0, 2, 0);
    question(&p, "unrelated.local", 1, 0x8001);
    question(&p, "sim-hub.local", 1, 1);
    CT_EQ(want_of(&p, &qu), HP_MDNS_A); CT_CHECK(!qu);
}

static void test_compression(void) {
    pk_t p;
    hdr(&p, 0, 2, 0);
    question(&p, "_haharmony._tcp.local", 12, 1);
    p8(&p, 0xC0); p8(&p, 12); /* second question: pointer to the first name */
    p16(&p, 12); p16(&p, 1);
    CT_EQ(want_of(&p, NULL), HP_MDNS_PTR);
    hp_mdns_query_t q;
    /* self pointer */
    hdr(&p, 0, 1, 0); p8(&p, 0xC0); p8(&p, 12); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
    /* forward pointer */
    hdr(&p, 0, 1, 0); p8(&p, 0xC0); p8(&p, 40); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
    /* mutual loop: label "a" -> ptr to itself-containing name via later->earlier chain */
    hdr(&p, 0, 1, 0);
    p8(&p, 1); p8(&p, 'a'); p8(&p, 0xC0); p8(&p, 12); /* name at 12 points to 12: loop */
    p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
    /* reserved label types */
    hdr(&p, 0, 1, 0); p8(&p, 0x40); p8(&p, 0); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
    hdr(&p, 0, 1, 0); p8(&p, 0x80); p8(&p, 0); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
}

static void test_name_limits(void) {
    pk_t p; hp_mdns_query_t q;
    /* 5 labels of 63 = 320 bytes > 255 */
    hdr(&p, 0, 1, 0);
    for (int i = 0; i < 5; i++) { p8(&p, 63); memset(p.b + p.n, 'a', 63); p.n += 63; }
    p8(&p, 0); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), -HP_E_INVAL);
    /* a 63-label name is fine */
    hdr(&p, 0, 1, 0);
    p8(&p, 63); memset(p.b + p.n, 'a', 63); p.n += 63; p8(&p, 0); p16(&p, 1); p16(&p, 1);
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), 0);
    /* hub_id limits */
    hp_mdns_svc_t t;
    char id[80];
    memset(id, 'x', 63); id[63] = 0;
    CT_EQ(hp_mdns_svc_init(&t, id, 1, IP, 1), 0);
    memset(id, 'x', 64); id[64] = 0;
    CT_EQ(hp_mdns_svc_init(&t, id, 1, IP, 1), -HP_E_INVAL);
    CT_EQ(hp_mdns_svc_init(&t, "", 1, IP, 1), -HP_E_INVAL);
    CT_EQ(hp_mdns_svc_init(&t, "a.b", 1, IP, 1), -HP_E_INVAL);
    CT_EQ(hp_mdns_svc_init(&t, "a b", 1, IP, 1), -HP_E_INVAL);
    CT_EQ(hp_mdns_svc_init(&t, NULL, 1, IP, 1), -HP_E_INVAL);
}

static void test_hostile(void) {
    hp_mdns_query_t q;
    pk_t p = query1("_haharmony._tcp.local", 12, 1);
    for (size_t n = 0; n <= p.n; n++) { /* every truncation: must not crash */
        int r = hp_mdns_parse_query(&S, p.b, n, &q);
        CT_CHECK(r == 0 || r == -HP_E_INVAL);
        if (n < p.n) CT_EQ(r, -HP_E_INVAL);
    }
    pk_t resp = p; resp.b[2] = 0x84; /* QR set */
    CT_EQ(hp_mdns_parse_query(&S, resp.b, resp.n, &q), -HP_E_INVAL);
    pk_t op = p; op.b[2] = 0x28; /* opcode 5 */
    CT_EQ(hp_mdns_parse_query(&S, op.b, op.n, &q), -HP_E_INVAL);
    /* qdcount claims more than present */
    pk_t big = p; big.b[5] = 50;
    CT_EQ(hp_mdns_parse_query(&S, big.b, big.n, &q), -HP_E_INVAL);
    CT_EQ(hp_mdns_parse_query(&S, NULL, 0, &q), -HP_E_INVAL);
    uint32_t x = 12345;
    for (int i = 0; i < 20000; i++) { /* garbage + mutated valid packets */
        pk_t g;
        g.n = (x >> 8) % 120;
        for (size_t k = 0; k < g.n; k++) { x = x * 1664525u + 1013904223u; g.b[k] = (uint8_t)(x >> 24); }
        if (i & 1) { memcpy(g.b, p.b, g.n < p.n ? g.n : p.n); if (g.n > 12) g.b[(x >> 5) % g.n] ^= (uint8_t)(x >> 9); }
        if (g.n >= 4) g.b[2] &= 0x7F;
        (void)hp_mdns_parse_query(&S, g.b, g.n, &q);
    }
}

static void test_known_answers(void) {
    pk_t p;
    hdr(&p, 0, 1, 1);
    question(&p, "_haharmony._tcp.local", 12, 1);
    pname(&p, "_haharmony._tcp.local"); p16(&p, 12); p16(&p, 1); p32(&p, 4500);
    size_t rl = p.n; p16(&p, 0);
    size_t st = p.n; pname(&p, "sim-hub._haharmony._tcp.local");
    p.b[rl] = (uint8_t)((p.n - st) >> 8); p.b[rl + 1] = (uint8_t)(p.n - st);
    CT_EQ(want_of(&p, NULL), 0); /* suppressed */
    /* same but TTL below half: must still answer */
    pk_t low;
    hdr(&low, 0, 1, 1);
    question(&low, "_haharmony._tcp.local", 12, 1);
    pname(&low, "_haharmony._tcp.local"); p16(&low, 12); p16(&low, 1); p32(&low, 100);
    rl = low.n; p16(&low, 0); st = low.n; pname(&low, "sim-hub._haharmony._tcp.local");
    low.b[rl] = (uint8_t)((low.n - st) >> 8); low.b[rl + 1] = (uint8_t)(low.n - st);
    CT_EQ(want_of(&low, NULL), HP_MDNS_PTR);
    /* truncated answer section: query still valid, still answered */
    pk_t tr = low; tr.n -= 5;
    CT_EQ(want_of(&tr, NULL), HP_MDNS_PTR);
}

/* ---- response decoding (test-local, no compression expected) ---- */

typedef struct { char name[256]; unsigned type, cls; uint32_t ttl; const uint8_t *rd; size_t rdlen; } rr_t;

static size_t dname(const uint8_t *b, size_t n, size_t off, char *out) {
    size_t o = 0;
    while (off < n && b[off]) {
        size_t l = b[off++];
        CT_CHECK((l & 0xC0) == 0); /* we never compress */
        CT_CHECK(off + l <= n);
        memcpy(out + o, b + off, l); o += l; out[o++] = '.'; off += l;
    }
    out[o] = 0;
    return off + 1;
}
static unsigned r16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) << 16 | r16(p + 2); }

static size_t drr(const uint8_t *b, size_t n, size_t off, rr_t *r) {
    off = dname(b, n, off, r->name);
    r->type = r16(b + off); r->cls = r16(b + off + 2); r->ttl = r32(b + off + 4);
    r->rdlen = r16(b + off + 8); r->rd = b + off + 10;
    CT_CHECK(off + 10 + r->rdlen <= n);
    return off + 10 + r->rdlen;
}

static const rr_t *find(const rr_t *rs, int n, unsigned type, const char *name) {
    for (int i = 0; i < n; i++)
        if (rs[i].type == type && !strcmp(rs[i].name, name)) return &rs[i];
    return NULL;
}

static int decode(const uint8_t *b, size_t n, rr_t *rs, unsigned *an, unsigned *ar) {
    CT_CHECK(n >= 12);
    CT_EQ(r16(b + 2), 0x8400);
    CT_EQ(r16(b + 4), 0);
    *an = r16(b + 6); *ar = r16(b + 10);
    size_t off = 12;
    int cnt = 0;
    for (unsigned i = 0; i < *an + *ar; i++) { off = drr(b, n, off, &rs[cnt++]); }
    CT_EQ(off, n);
    return cnt;
}

static void test_build(void) {
    uint8_t buf[1500];
    rr_t rs[16];
    unsigned an, ar;
    int n = hp_mdns_build_response(&S, HP_MDNS_PTR, 0, 0, buf, sizeof buf);
    CT_CHECK(n > 0);
    int cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 1); CT_EQ(ar, 3); CT_EQ(cnt, 4);
    const rr_t *ptr = find(rs, cnt, 12, "_haharmony._tcp.local.");
    CT_CHECK(ptr);
    if (ptr) {
        char t[256];
        dname(ptr->rd, ptr->rdlen, 0, t);
        CT_CHECK(!strcmp(t, "sim-hub._haharmony._tcp.local."));
        CT_EQ(ptr->ttl, 4500); CT_EQ(ptr->cls, 1); /* shared: no cache-flush */
    }
    const rr_t *srv = find(rs, cnt, 33, "sim-hub._haharmony._tcp.local.");
    CT_CHECK(srv);
    if (srv) {
        CT_EQ(srv->ttl, 120); CT_EQ(srv->cls, 0x8001);
        CT_EQ(r16(srv->rd + 4), 8099);
        char t[256]; dname(srv->rd + 6, srv->rdlen - 6, 0, t);
        CT_CHECK(!strcmp(t, "sim-hub.local."));
    }
    const rr_t *txt = find(rs, cnt, 16, "sim-hub._haharmony._tcp.local.");
    CT_CHECK(txt);
    if (txt) {
        CT_EQ(txt->ttl, 4500); CT_EQ(txt->cls, 0x8001);
        CT_EQ(txt->rd[0], 14); CT_MEM_EQ(txt->rd + 1, "hub_id=sim-hub", 14);
        CT_EQ(txt->rd[15], 3); CT_MEM_EQ(txt->rd + 16, "v=1", 3);
        CT_EQ(txt->rdlen, 19);
    }
    const rr_t *a = find(rs, cnt, 1, "sim-hub.local.");
    CT_CHECK(a);
    if (a) { CT_EQ(a->ttl, 120); CT_EQ(a->cls, 0x8001); CT_EQ(a->rdlen, 4); CT_EQ(r32(a->rd), IP); }

    n = hp_mdns_build_response(&S, HP_MDNS_ENUM, 0, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 1); CT_EQ(ar, 0);
    CT_CHECK(find(rs, cnt, 12, "_services._dns-sd._udp.local."));

    n = hp_mdns_build_response(&S, HP_MDNS_SRV, 0, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 1); CT_EQ(ar, 1); CT_CHECK(find(rs, cnt, 1, "sim-hub.local."));

    n = hp_mdns_build_response(&S, HP_MDNS_TXT, 0, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 1); CT_EQ(ar, 0);

    n = hp_mdns_build_response(&S, HP_MDNS_A, 0, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 1); CT_EQ(ar, 0);

    /* all five in one answer (announcement) */
    n = hp_mdns_build_response(&S, HP_MDNS_ALL, 0, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(an, 5); CT_EQ(ar, 0);

    /* goodbye: every TTL 0 */
    n = hp_mdns_build_response(&S, HP_MDNS_ALL, HP_MDNS_F_GOODBYE, 0, buf, sizeof buf);
    cnt = decode(buf, (size_t)n, rs, &an, &ar);
    CT_EQ(cnt, 5);
    for (int i = 0; i < cnt; i++) CT_EQ(rs[i].ttl, 0);

    /* legacy unicast: id echoed, ttl <= 10, no cache-flush */
    n = hp_mdns_build_response(&S, HP_MDNS_PTR, HP_MDNS_F_LEGACY, 0xBEEF, buf, sizeof buf);
    CT_EQ(r16(buf), 0xBEEF);
    size_t off = 12;
    for (int i = 0; i < 4; i++) {
        rr_t r; off = drr(buf, (size_t)n, off, &r);
        CT_CHECK(r.ttl == 10); CT_EQ(r.cls, 1);
    }
    /* non-legacy: id 0 */
    n = hp_mdns_build_response(&S, HP_MDNS_PTR, 0, 0xBEEF, buf, sizeof buf);
    CT_EQ(r16(buf), 0);

    /* every too-small buffer fails cleanly, never overruns (ASan) */
    int full = hp_mdns_build_response(&S, HP_MDNS_ALL, 0, 0, buf, sizeof buf);
    for (int cap = 0; cap < full; cap++) {
        uint8_t *small = malloc((size_t)cap ? (size_t)cap : 1);
        CT_EQ(hp_mdns_build_response(&S, HP_MDNS_ALL, 0, 0, small, (size_t)cap), -HP_E_TOOBIG);
        free(small);
    }
}

static void test_max_name_build(void) {
    hp_mdns_svc_t t;
    char id[64];
    memset(id, 'h', 63); id[63] = 0;
    CT_EQ(hp_mdns_svc_init(&t, id, 80, IP, 1), 0);
    uint8_t buf[1500];
    int n = hp_mdns_build_response(&t, HP_MDNS_ALL, 0, 0, buf, sizeof buf);
    CT_CHECK(n > 0 && n < 1500);
    /* a query for its own names round-trips */
    pk_t p; hdr(&p, 0, 1, 0);
    pname(&p, t.host); /* wire name from dotted text */
    p.n -= 0;
    p16(&p, 1); p16(&p, 1);
    hp_mdns_query_t q;
    CT_EQ(hp_mdns_parse_query(&t, p.b, p.n, &q), 0);
    CT_EQ(q.want, HP_MDNS_A);
}

static void test_legacy_echo(void) {
    pk_t p = query1("_HaHarmony._tcp.local", 12, 1); /* mixed case, no QU */
    hp_mdns_query_t q;
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), 0);
    CT_CHECK(q.have_q);
    uint8_t buf[1500];
    int n = hp_mdns_build_response_q(&S, q.want, HP_MDNS_F_LEGACY, q.id, &q, buf, sizeof buf);
    CT_CHECK(n > 12);
    CT_EQ(r16(buf), 0x1234); CT_EQ(r16(buf + 4), 1); CT_EQ(r16(buf + 6), 1);
    size_t qlen = p.n - 12; /* the question bytes as sent */
    CT_MEM_EQ(buf + 12, p.b + 12, qlen);
    /* the answer follows the echoed question and decodes */
    rr_t r; drr(buf, (size_t)n, 12 + qlen, &r);
    CT_EQ(r.type, 12);
    /* non-legacy / no question: QDCOUNT 0 */
    CT_EQ(hp_mdns_parse_query(&S, p.b, p.n, &q), 0);
    n = hp_mdns_build_response_q(&S, HP_MDNS_PTR, 0, 0, &q, buf, sizeof buf);
    CT_EQ(r16(buf + 4), 0);
    n = hp_mdns_build_response_q(&S, HP_MDNS_PTR, HP_MDNS_F_LEGACY, 7, NULL, buf, sizeof buf);
    CT_EQ(r16(buf + 4), 0);
}

static void test_on_link(void) {
    uint32_t me = 0x0A0A4D7E, m24 = 0xFFFFFF00; /* 198.51.100.126/24 */
    CT_CHECK(hp_mdns_on_link(0x0A0A4D01, me, m24));
    CT_CHECK(hp_mdns_on_link(me, me, m24));
    CT_CHECK(!hp_mdns_on_link(0x0A0A4E01, me, m24)); /* neighbouring /24 */
    CT_CHECK(!hp_mdns_on_link(0x08080808, me, m24)); /* off-link / spoofed */
    CT_CHECK(hp_mdns_on_link(0x7F000001, me, m24));  /* loopback always */
    CT_CHECK(hp_mdns_on_link(0x7F010203, me, m24));
    CT_CHECK(!hp_mdns_on_link(0x08080808, me, 0xFFFFFFFFu)); /* unknown mask: fail closed */
    CT_CHECK(!hp_mdns_on_link(0, me, m24));
}

static void test_holdoff(void) {
    hp_mdns_rl_t rl; hp_mdns_rl_init(&rl);
    CT_EQ(hp_mdns_rl_take(&rl, 5000, HP_MDNS_PTR | HP_MDNS_A), HP_MDNS_PTR | HP_MDNS_A);
    CT_EQ(hp_mdns_rl_take(&rl, 5500, HP_MDNS_PTR | HP_MDNS_SRV), HP_MDNS_SRV); /* PTR held off */
    CT_EQ(hp_mdns_rl_take(&rl, 5999, HP_MDNS_PTR), 0);
    CT_EQ(hp_mdns_rl_take(&rl, 6000, HP_MDNS_PTR), HP_MDNS_PTR); /* exactly 1 s later */
    CT_EQ(hp_mdns_rl_take(&rl, 6000, HP_MDNS_PTR), 0);
    /* wraparound: sent just before 2^32, next just after */
    hp_mdns_rl_init(&rl);
    CT_EQ(hp_mdns_rl_take(&rl, 0xFFFFFF00u, HP_MDNS_TXT), HP_MDNS_TXT);
    CT_EQ(hp_mdns_rl_take(&rl, 0x000000F0u, HP_MDNS_TXT), 0);            /* 496 ms later */
    CT_EQ(hp_mdns_rl_take(&rl, 0x00000500u, HP_MDNS_TXT), HP_MDNS_TXT);  /* 1.5 s later */
    /* a stale stamp (~2^31 ms old) must never block: diff is huge, not in the window */
    hp_mdns_rl_init(&rl);
    CT_EQ(hp_mdns_rl_take(&rl, 100, HP_MDNS_ENUM), HP_MDNS_ENUM);
    CT_EQ(hp_mdns_rl_take(&rl, 100u + 0x80000000u, HP_MDNS_ENUM), HP_MDNS_ENUM);
    /* clock stepping backwards: (now-then) wraps huge -> not held off */
    hp_mdns_rl_init(&rl);
    CT_EQ(hp_mdns_rl_take(&rl, 10000, HP_MDNS_A), HP_MDNS_A);
    CT_EQ(hp_mdns_rl_take(&rl, 9000, HP_MDNS_A), HP_MDNS_A);
    /* sweep drops expired stamps so a 49-day-old one cannot alias into the window */
    hp_mdns_rl_init(&rl);
    CT_EQ(hp_mdns_rl_take(&rl, 1000, HP_MDNS_A), HP_MDNS_A);
    hp_mdns_rl_sweep(&rl, 3000);
    CT_EQ(rl.have, 0);
    CT_EQ(hp_mdns_rl_take(&rl, 1000u + 0xFFFFFFFFu, HP_MDNS_A), HP_MDNS_A);
}

static uint32_t g_now;
static uint32_t fake_clock(void *c) { (void)c; return g_now; }
static int g_detect_ok; static uint32_t g_ip, g_mask;
static int fake_detect(uint32_t *ip, uint32_t *mask) {
    if (!g_detect_ok) return -1;
    *ip = g_ip; *mask = g_mask;
    return 0;
}
/* Advances the fake clock to the next timer and runs it; returns the wait. */
static int tick(hp_sched_t *sc) {
    int w = hp_sched_next_timeout_ms(sc);
    if (w < 0) return w;
    g_now += (uint32_t)w;
    hp_sched_run(sc, g_now);
    return w;
}

static void test_backoff_fn(void) {
    uint32_t d = HP_MDNS_RETRY_MIN_MS;
    CT_EQ(d, 1000);
    d = hp_mdns_backoff_next(d); CT_EQ(d, 2000);
    d = hp_mdns_backoff_next(d); CT_EQ(d, 4000);
    for (int i = 0; i < 10; i++) d = hp_mdns_backoff_next(d);
    CT_EQ(d, HP_MDNS_RETRY_MAX_MS);
    CT_EQ(hp_mdns_backoff_next(0xFFFFFFFFu), HP_MDNS_RETRY_MAX_MS);
}

static void test_retry_state_machine(void) {
    hp_sched_t sc; g_now = 0xFFFFF000u; /* start near wraparound */
    hp_sched_init(&sc, fake_clock, NULL);
    hp_mdns_opts_t o = {0, NULL, "127.0.0.1:9", fake_detect};
    hp_mdns_srv_t m;
    g_detect_ok = 0; g_mask = 0xFF000000u; g_ip = 0x7F000001u;
    /* no address yet: start succeeds soft, no socket, retries with backoff */
    CT_EQ(hp_mdns_srv_start(&m, &sc, &o, "sim-hub", 8099, 1), 0);
    CT_EQ(m.fd, -1);
    int expect[] = {1000,2000, 4000, 8000, 16000, 32000, 60000, 60000};
    for (unsigned i = 0; i < sizeof expect / sizeof *expect; i++) {
        int w = hp_sched_next_timeout_ms(&sc);
        CT_CHECK(w >= 0 && w <= (int)HP_MDNS_RETRY_MAX_MS);
        CT_EQ(w, expect[i]);
        tick(&sc);
        CT_EQ(m.fd, -1);
    }
    CT_EQ(hp_sched_pending(&sc), 1);
    /* address appears: comes up, announces, then rechecks every 30 s */
    g_detect_ok = 1;
    tick(&sc);
    if (m.fd < 0) { /* host cannot join on lo: state machine still retrying */
        fprintf(stderr, "note: group join unavailable on lo, up-path skipped\n");
        CT_EQ(hp_sched_pending(&sc), 1);
        hp_mdns_srv_stop(&m);
        return;
    }
    CT_EQ(m.svc.ipv4, 0x7F000001u);
    while (hp_sched_pending(&sc) > 1) tick(&sc); /* drain announcements */
    CT_EQ(hp_sched_next_timeout_ms(&sc) > 0 && hp_sched_next_timeout_ms(&sc) <= 30000, 1);
    /* address changed: rebinds with the new A record */
    g_ip = 0x7F000002u;
    while (m.svc.ipv4 != 0x7F000002u) { if (tick(&sc) < 0) break; }
    CT_EQ(m.svc.ipv4, 0x7F000002u);
    CT_CHECK(m.fd >= 0);
    CT_CHECK(hp_sched_pending(&sc) >= 1); /* re-announce and/or recheck */
    /* address lost: socket closed, retry from 1 s */
    g_detect_ok = 0;
    for (int i = 0; i < 4 && m.fd >= 0; i++) tick(&sc);
    CT_EQ(m.fd, -1);
    while (hp_sched_pending(&sc) > 1) tick(&sc);
    CT_EQ(hp_sched_next_timeout_ms(&sc), 1000);
    hp_mdns_srv_stop(&m); /* stop while down cancels the retry timer */
    CT_EQ(hp_sched_pending(&sc), 0);
}

int main(void) {
    CT_EQ(hp_mdns_svc_init(&S, "sim-hub", 8099, IP, 1), 0);
    CT_RUN(test_questions);
    CT_RUN(test_compression);
    CT_RUN(test_name_limits);
    CT_RUN(test_hostile);
    CT_RUN(test_known_answers);
    CT_RUN(test_build);
    CT_RUN(test_max_name_build);
    CT_RUN(test_legacy_echo);
    CT_RUN(test_on_link);
    CT_RUN(test_holdoff);
    CT_RUN(test_backoff_fn);
    CT_RUN(test_retry_state_machine);
    return CT_DONE();
}
