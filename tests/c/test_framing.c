/* RF object codec: golden vectors (tests/vectors/rf/frames.json, argv[1] = file), property
 * loops, fault injection, and the stop-and-wait sender against a lossy simulated link.
 * Pure and deterministic: no sockets, no sleeps, no real clock. */
#include "ctest_assert.h"
#include "cJSON.h"
#include "hub/hp_errors.h"
#include "hub/rf/framing.h"

#define T_KEY HP_MSG_KEY_EVENT

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + 3);
}

/* ---- hex / vectors ---- */

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s) / 2;
    if (n > cap) return (size_t)-1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static void check_vector(const cJSON *v)
{
    const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
    uint8_t type = (uint8_t)cJSON_GetObjectItem(v, "type")->valueint;
    uint8_t seq = (uint8_t)cJSON_GetObjectItem(v, "seq")->valueint;
    uint8_t slot = (uint8_t)cJSON_GetObjectItem(v, "slot")->valueint;
    uint8_t power = (uint8_t)cJSON_GetObjectItem(v, "power")->valueint;
    uint8_t body[256], want[64];
    size_t blen = unhex(cJSON_GetObjectItem(v, "body")->valuestring, body, sizeof body);
    const cJSON *inner = cJSON_GetObjectItem(v, "inner"), *hub = cJSON_GetObjectItem(v, "hub");
    const cJSON *rem = cJSON_GetObjectItem(v, "remote");
    hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
    size_t n = 0;
    hp_rf_asm_t a, ar;
    hp_rf_msg_t msg;
    hp_rf_inner_t ack, in;

    if (blen == (size_t)-1) { fprintf(stderr, "%s: bad body\n", name); ct_failures++; return; }
    CT_EQ(hp_rf_split((hp_msg_type_t)type, seq, body, blen, fr, HP_RF_MAX_FRAGS, &n), 0);
    CT_EQ(n, cJSON_GetArraySize(inner));
    CT_EQ(n, cJSON_GetArraySize(hub));
    CT_EQ(n, cJSON_GetArraySize(rem));
    hp_rf_asm_init(&a);
    hp_rf_asm_init(&ar);
    for (size_t i = 0; i < n; i++) {
        hp_rf_frame_t w;
        uint8_t s2, b1;
        size_t wl = unhex(cJSON_GetArrayItem(inner, (int)i)->valuestring, want, sizeof want);
        CT_EQ(wl, HP_RF_INNER_LEN);
        CT_MEM_EQ(fr[i].b, want, HP_RF_INNER_LEN);

        hp_rf_wrap_hub(slot, &fr[i], &w);
        wl = unhex(cJSON_GetArrayItem(hub, (int)i)->valuestring, want, sizeof want);
        CT_EQ(w.len, wl);
        CT_EQ(w.len, HP_RF_HUB_FRAME_LEN);
        CT_MEM_EQ(w.data, want, wl);
        CT_EQ(hp_rf_unwrap_hub(want, wl, &s2, &in), 0);
        CT_EQ(s2, slot);
        CT_MEM_EQ(in.b, fr[i].b, HP_RF_INNER_LEN);
        int r = hp_rf_asm_push(&a, &in, &msg, &ack);
        CT_EQ(r, i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE);

        hp_rf_wrap_remote(slot, power, &fr[i], &w);
        wl = unhex(cJSON_GetArrayItem(rem, (int)i)->valuestring, want, sizeof want);
        CT_EQ(w.len, wl);
        CT_EQ(w.len, HP_RF_REMOTE_FRAME_LEN);
        CT_MEM_EQ(w.data, want, wl);
        CT_EQ(hp_rf_unwrap_remote(want, wl, &s2, &b1, &in), 0);
        CT_EQ(s2, slot);
        CT_EQ(b1, (slot & 0xf) | power);
        CT_MEM_EQ(in.b, fr[i].b, HP_RF_INNER_LEN);
        r = hp_rf_asm_push(&ar, &in, &msg, &ack);
        CT_EQ(r, i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE);
    }
    CT_EQ(msg.type, type);
    CT_EQ(msg.seq, seq);
    CT_EQ(msg.len, blen);
    CT_MEM_EQ(msg.payload, body, blen);
    hp_rf_make_ack(type, seq, &ack);
    size_t al = unhex(cJSON_GetObjectItem(v, "ack")->valuestring, want, sizeof want);
    CT_EQ(al, HP_RF_INNER_LEN);
    CT_MEM_EQ(ack.b, want, HP_RF_INNER_LEN);
}

static void test_vectors(const char *path)
{
    char *txt = slurp(path);
    CT_CHECK(txt != NULL);
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    CT_CHECK(root != NULL);
    if (!root) { free(txt); return; }
    const cJSON *vs = cJSON_GetObjectItem(root, "vectors"), *rj = cJSON_GetObjectItem(root, "reject"), *e;
    CT_CHECK(cJSON_GetArraySize(vs) >= 7);
    cJSON_ArrayForEach(e, vs) check_vector(e);
    /* Measured on the device (hub-rf-cc2544.md s8a): 0x10/0x11 control frames are not data. */
    cJSON_ArrayForEach(e, rj) {
        uint8_t f[64], slot, b1;
        hp_rf_inner_t in;
        size_t n = unhex(cJSON_GetObjectItem(e, "frame")->valuestring, f, sizeof f);
        const char *name = cJSON_GetObjectItem(e, "name")->valuestring;
        if (hp_rf_unwrap_hub(f, n, &slot, &in) != -HP_E_INVAL) { fprintf(stderr, "hub accepted %s\n", name); ct_failures++; }
        if (hp_rf_unwrap_remote(f, n, &slot, &b1, &in) != -HP_E_INVAL) { fprintf(stderr, "remote accepted %s\n", name); ct_failures++; }
    }
    cJSON_Delete(root);
    free(txt);
}

/* ---- round-trips ---- */

static void test_every_length(void)
{
    for (size_t len = 0; len <= HP_RF_BODY_MAX; len++) {
        uint8_t *body = malloc(len ? len : 1); /* exact size: ASan catches over-reads */
        hp_rf_inner_t *fr = malloc(HP_RF_MAX_FRAGS * sizeof *fr);
        size_t n = 0, want = (len + 1 + 24) / 25;
        hp_rf_asm_t a;
        hp_rf_msg_t msg;
        hp_rf_inner_t ack;
        fill(body, len);
        CT_EQ(hp_rf_split(T_KEY, (uint8_t)len, body, len, fr, HP_RF_MAX_FRAGS, &n), 0);
        CT_EQ(n, want);
        hp_rf_asm_init(&a);
        for (size_t i = 0; i < n; i++) {
            /* header discipline */
            uint8_t h = fr[i].b[0];
            CT_EQ(h & 0x0f, len & 0x0f);
            CT_EQ(h & 0xe0, i ? HP_RF_KIND_CONT : HP_RF_KIND_OBJECT);
            CT_EQ(!!(h & HP_RF_FLAG_FIRST), i == 0);
            CT_EQ(fr[i].b[1], T_KEY);
            /* zero padding after the last byte */
            if (i + 1 == n)
                for (size_t k = 3 + ((len + 1) - 25 * i); k < HP_RF_INNER_LEN; k++) CT_EQ(fr[i].b[k], 0);
            int r = hp_rf_asm_push(&a, &fr[i], &msg, &ack);
            CT_EQ(r, i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE);
        }
        CT_EQ(msg.len, len);
        CT_EQ(msg.seq, len & 0x0f);
        CT_EQ(msg.type, T_KEY);
        CT_MEM_EQ(msg.payload, body, len);
        CT_EQ(ack.b[0], HP_RF_KIND_ACK | (len & 0x0f));
        CT_EQ(ack.b[1], T_KEY);
        free(body);
        free(fr);
    }
}

static void test_split_limits(void)
{
    uint8_t p[300] = {0};
    hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
    size_t n = 99;
    CT_EQ(hp_rf_split(T_KEY, 1, p, 255, fr, HP_RF_MAX_FRAGS, &n), -HP_E_TOOBIG);
    CT_EQ(hp_rf_split(T_KEY, 1, p, 254, fr, 10, &n), -HP_E_TOOBIG);
    CT_EQ(hp_rf_split(T_KEY, 1, NULL, 4, fr, HP_RF_MAX_FRAGS, &n), -HP_E_INVAL);
    CT_EQ(hp_rf_split((hp_msg_type_t)0x100, 1, p, 4, fr, HP_RF_MAX_FRAGS, &n), -HP_E_INVAL);
    CT_EQ(n, 99);
    CT_EQ(hp_rf_split(T_KEY, 0x1f, p, 0, fr, HP_RF_MAX_FRAGS, &n), 0); /* seq masked to 4 bits */
    CT_EQ(fr[0].b[0], 0x3f);
}

static void test_outer_wrap(void)
{
    hp_rf_inner_t in, out;
    hp_rf_frame_t w;
    uint8_t slot, b1;
    for (size_t i = 0; i < HP_RF_INNER_LEN; i++) in.b[i] = (uint8_t)(0xa0 + i);
    hp_rf_wrap_hub(3, &in, &w);
    CT_EQ(w.len, 30);
    CT_EQ(w.data[0], 0x12);
    CT_EQ(w.data[1], 3);
    CT_EQ(hp_rf_unwrap_hub(w.data, 29, &slot, &out), -HP_E_INVAL);
    CT_EQ(hp_rf_unwrap_hub(w.data, 31, &slot, &out), -HP_E_INVAL);
    w.data[0] = 0x13;
    CT_EQ(hp_rf_unwrap_hub(w.data, 30, &slot, &out), -HP_E_INVAL);
    hp_rf_wrap_remote(0x13, 0x40, &in, &w);
    CT_EQ(w.len, 32);
    CT_EQ(w.data[0], 0x20);
    CT_EQ(w.data[1], 0x43); /* (slot & 0xf) | powermode */
    CT_EQ(w.data[2], 0x12);
    CT_EQ(w.data[3], 0x13);
    CT_EQ(hp_rf_unwrap_remote(w.data, 31, &slot, &b1, &out), -HP_E_INVAL);
    CT_EQ(hp_rf_unwrap_remote(w.data, 32, &slot, &b1, &out), 0);
    CT_EQ(slot, 0x13);
    CT_MEM_EQ(out.b, in.b, HP_RF_INNER_LEN);
    CT_EQ(hp_rf_unwrap_hub(w.data, 32, &slot, &out), -HP_E_INVAL);
    w.data[0] = 0x12;
    CT_EQ(hp_rf_unwrap_remote(w.data, 32, &slot, &b1, &out), -HP_E_INVAL);
}

/* ---- receive-side faults ---- */

static hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
static hp_rf_msg_t msg;
static hp_rf_inner_t ackf;

static size_t make(size_t len, uint8_t seq)
{
    uint8_t p[HP_RF_BODY_MAX];
    size_t n = 0;
    fill(p, len);
    CT_EQ(hp_rf_split(T_KEY, seq, p, len, fr, HP_RF_MAX_FRAGS, &n), 0);
    return n;
}

static int push(hp_rf_asm_t *a, size_t i)
{
    return hp_rf_asm_push(a, &fr[i], &msg, &ackf);
}

static void test_lost_first_frame(void)
{
    hp_rf_asm_t a;
    size_t n = make(80, 3); /* 4 frames */
    hp_rf_asm_init(&a);
    CT_EQ(n, 4);
    for (size_t i = 1; i < n; i++) CT_EQ(push(&a, i), -HP_E_INVAL);
    for (size_t i = 0; i < n; i++) CT_EQ(push(&a, i), i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE); /* retry */
}

static void test_lost_continuation(void)
{
    for (size_t lost = 1; lost < 4; lost++) {
        hp_rf_asm_t a;
        size_t n = make(80, 3);
        hp_rf_asm_init(&a);
        int completed = 0;
        for (size_t i = 0; i < n; i++)
            if (i != lost) completed |= push(&a, i) == HP_ASM_COMPLETE;
        CT_EQ(completed, 0);
        for (size_t i = 0; i < n; i++) CT_EQ(push(&a, i), i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE);
        CT_EQ(msg.len, 80);
    }
}

static void test_duplicated_frame(void)
{
    for (size_t dup = 0; dup < 4; dup++) {
        hp_rf_asm_t a;
        size_t n = make(80, 3);
        hp_rf_asm_init(&a);
        int completes = 0;
        for (size_t i = 0; i < n; i++) {
            completes += push(&a, i) == HP_ASM_COMPLETE;
            if (i == dup) completes += push(&a, i) == HP_ASM_COMPLETE;
        }
        /* A repeated continuation is rejected (so no object, no wrong data); a repeated
         * first frame just restarts and the object still completes exactly once. */
        if (dup == 0 || dup == 3) CT_EQ(completes, 1); /* dup == 3: the extra tail is rejected */
        else CT_EQ(completes, 0);
    }
    /* A repeated LAST frame arrives after delivery: rejected, nothing redelivered. */
    hp_rf_asm_t a;
    size_t n = make(80, 3);
    hp_rf_asm_init(&a);
    for (size_t i = 0; i < n; i++) push(&a, i);
    CT_EQ(push(&a, n - 1), -HP_E_INVAL);
}

static void test_reordered_frames(void)
{
    static const size_t orders[][4] = {{0, 2, 1, 3}, {1, 0, 2, 3}, {0, 1, 3, 2}, {3, 2, 1, 0}, {0, 3, 2, 1}};
    for (size_t o = 0; o < sizeof orders / sizeof *orders; o++) {
        hp_rf_asm_t a;
        size_t n = make(80, 3);
        int completes = 0;
        hp_rf_asm_init(&a);
        CT_EQ(n, 4);
        for (size_t i = 0; i < n; i++) completes += push(&a, orders[o][i]) == HP_ASM_COMPLETE;
        CT_EQ(completes, 0);
    }
}

static void test_corrupted_kind_bits(void)
{
    static const uint8_t bits[] = {0x10, 0x20, 0x40, 0x80};
    for (size_t i = 0; i < 4; i++)
        for (size_t b = 0; b < sizeof bits; b++) {
            hp_rf_asm_t a;
            size_t n = make(80, 3);
            int completes = 0;
            hp_rf_asm_init(&a);
            fr[i].b[0] ^= bits[b];
            for (size_t k = 0; k < n; k++) completes += push(&a, k) == HP_ASM_COMPLETE;
            if (completes) fprintf(stderr, "frame %zu bit 0x%02x delivered\n", i, bits[b]);
            CT_EQ(completes, 0);
        }
    /* corrupted seq / type on a continuation */
    for (int field = 0; field < 2; field++) {
        hp_rf_asm_t a;
        size_t n = make(80, 3);
        int completes = 0;
        hp_rf_asm_init(&a);
        if (field == 0) fr[2].b[0] ^= 0x01;
        else fr[2].b[1] ^= 0x01;
        for (size_t k = 0; k < n; k++) completes += push(&a, k) == HP_ASM_COMPLETE;
        CT_EQ(completes, 0);
    }
    /* a first frame whose length byte is 255 overflows len+body and is refused */
    hp_rf_asm_t a;
    make(10, 1);
    fr[0].b[3] = 255;
    hp_rf_asm_init(&a);
    CT_EQ(push(&a, 0), -HP_E_INVAL);
}

static void test_new_first_abandons_partial(void)
{
    hp_rf_asm_t a;
    hp_rf_inner_t second[HP_RF_MAX_FRAGS];
    size_t n = make(80, 3), n2 = 0;
    uint8_t p[40];
    fill(p, 40);
    for (size_t i = 0; i < 40; i++) p[i] ^= 0x55;
    CT_EQ(hp_rf_split(T_KEY, 4, p, 40, second, HP_RF_MAX_FRAGS, &n2), 0);
    hp_rf_asm_init(&a);
    CT_EQ(push(&a, 0), HP_ASM_INCOMPLETE);
    CT_EQ(push(&a, 1), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, &second[0], &msg, &ackf), HP_ASM_INCOMPLETE); /* different seq: new object */
    CT_EQ(push(&a, 2), -HP_E_INVAL);                                       /* old object's tail is orphaned */
    CT_EQ(n2, 2);
    CT_EQ(hp_rf_asm_push(&a, &second[0], &msg, &ackf), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, &second[1], &msg, &ackf), HP_ASM_COMPLETE);
    CT_EQ(msg.seq, 4);
    CT_EQ(msg.len, 40);
    CT_MEM_EQ(msg.payload, p, 40);
    (void)n;
}

static void test_duplicate_object_reacked_not_redelivered(void)
{
    hp_rf_asm_t a;
    size_t n = make(60, 9);
    hp_rf_asm_init(&a);
    for (size_t i = 0; i < n; i++) push(&a, i);
    CT_EQ(msg.len, 60);
    for (int retry = 0; retry < 3; retry++) {
        memset(&msg, 0xee, sizeof msg);
        for (size_t i = 0; i < n; i++) {
            int r = push(&a, i);
            CT_EQ(r, i + 1 == n ? HP_ASM_DUPLICATE : HP_ASM_INCOMPLETE);
        }
        CT_EQ(ackf.b[0], HP_RF_KIND_ACK | 9); /* re-acked */
        CT_EQ(ackf.b[1], T_KEY);
        CT_EQ(msg.seq, 0xee); /* nothing delivered */
    }
    /* a new object with a different seq is delivered; the old seq is then fresh again */
    size_t n2 = make(5, 10);
    for (size_t i = 0; i < n2; i++) CT_EQ(push(&a, i), HP_ASM_COMPLETE);
    n = make(60, 9);
    for (size_t i = 0; i < n; i++) push(&a, i);
    CT_EQ(msg.seq, 9);
    /* single-frame duplicates too */
    n = make(0, 2);
    CT_EQ(push(&a, 0), HP_ASM_COMPLETE);
    CT_EQ(push(&a, 0), HP_ASM_DUPLICATE);
    hp_rf_asm_init(&a); /* a link change forgets the last seq */
    CT_EQ(push(&a, 0), HP_ASM_COMPLETE);
}

static void test_ack_frame(void)
{
    hp_rf_asm_t a;
    hp_rf_inner_t ack, junk;
    hp_rf_asm_init(&a);
    hp_rf_make_ack(0x0d, 0x1b, &ack);
    CT_EQ(ack.b[0], 0x4b);
    CT_EQ(hp_rf_asm_push(&a, &ack, &msg, &junk), HP_ASM_ACK);
    CT_EQ(msg.type, 0x0d);
    CT_EQ(msg.seq, 0xb);
    memset(junk.b, 0, sizeof junk.b); /* kind 0x00: not ours */
    CT_EQ(hp_rf_asm_push(&a, &junk, &msg, &ackf), -HP_E_INVAL);
    junk.b[0] = 0x80;
    CT_EQ(hp_rf_asm_push(&a, &junk, &msg, &ackf), -HP_E_INVAL);
}

/* ---- sender against a lossy link ---- */

typedef struct {
    hp_rf_tx_t tx;
    hp_rf_asm_t rx;
    uint32_t now;
    int sent;                 /* data frames the sender put on the air */
    int delivered;
    int drop_data[64];        /* indices of data frames (0-based over `sent`) to drop */
    int ndrop;
    int drop_all;
    int drop_acks;            /* number of acks to lose */
    int acks_sent;
    int pend; /* an ack waiting to reach the sender */
    uint8_t pend_type, pend_seq;
    uint8_t last_seq;
    size_t last_len;
    uint8_t last_body[HP_RF_BODY_MAX];
} link_t;

static int link_send(void *ctx, const hp_rf_inner_t *f)
{
    link_t *l = ctx;
    int idx = l->sent++, drop = l->drop_all;
    for (int i = 0; i < l->ndrop; i++) drop |= l->drop_data[i] == idx;
    if (drop) return 0;
    hp_rf_msg_t m;
    hp_rf_inner_t ack;
    int r = hp_rf_asm_push(&l->rx, f, &m, &ack);
    if (r == HP_ASM_COMPLETE) {
        l->delivered++;
        l->last_seq = m.seq;
        l->last_len = m.len;
        memcpy(l->last_body, m.payload, m.len);
    }
    if (r == HP_ASM_COMPLETE || r == HP_ASM_DUPLICATE) {
        l->acks_sent++;
        if (l->drop_acks > 0) l->drop_acks--;
        else { l->pend = 1; l->pend_type = ack.b[1]; l->pend_seq = ack.b[0] & 0xf; }
    }
    return 0;
}

static void link_init(link_t *l)
{
    memset(l, 0, sizeof *l);
    hp_rf_asm_init(&l->rx);
    hp_rf_tx_init(&l->tx, link_send, l);
}

static void deliver_ack(link_t *l)
{
    if (l->pend) {
        l->pend = 0;
        hp_rf_tx_on_ack(&l->tx, l->pend_type, l->pend_seq);
    }
}

/* Advances the clock in 10 ms steps, delivering acks, until the sender settles. */
static hp_rf_tx_state_t settle(link_t *l, uint32_t max_ms)
{
    deliver_ack(l);
    for (uint32_t t = 0; t < max_ms && l->tx.state == HP_RF_TX_WAIT; t += 10) {
        l->now += 10;
        hp_rf_tx_poll(&l->tx, l->now);
        deliver_ack(l);
    }
    return l->tx.state;
}

static void test_tx_clean(void)
{
    link_t l;
    uint8_t p[60];
    fill(p, 60);
    link_init(&l);
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, p, 60, l.now), 0);
    CT_EQ(l.tx.state, HP_RF_TX_WAIT);
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, p, 60, l.now), -HP_E_BUSY);
    CT_EQ(settle(&l, 1000), HP_RF_TX_OK);
    CT_EQ(l.sent, 3);
    CT_EQ(l.delivered, 1);
    CT_EQ(l.last_len, 60);
    CT_MEM_EQ(l.last_body, p, 60);
    CT_EQ(hp_rf_tx_on_ack(&l.tx, T_KEY, 0), 0); /* no object in flight */
}

static void test_tx_timing(void)
{
    link_t l;
    link_init(&l);
    l.drop_all = 1;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, (const uint8_t *)"x", 1, 1000), 0);
    CT_EQ(l.sent, 1);
    hp_rf_tx_poll(&l.tx, 1149);
    CT_EQ(l.sent, 1); /* not yet */
    hp_rf_tx_poll(&l.tx, 1150);
    CT_EQ(l.sent, 2);
    hp_rf_tx_poll(&l.tx, 1299);
    CT_EQ(l.sent, 2);
    hp_rf_tx_poll(&l.tx, 1300);
    CT_EQ(l.sent, 3);
    CT_EQ(hp_rf_tx_poll(&l.tx, 1450), HP_RF_TX_WAIT);
    CT_EQ(l.sent, 4); /* third and last retry */
    CT_EQ(hp_rf_tx_poll(&l.tx, 1599), HP_RF_TX_WAIT);
    CT_EQ(hp_rf_tx_poll(&l.tx, 1600), HP_RF_TX_FAILED);
    CT_EQ(l.sent, 4);
    CT_EQ(l.delivered, 0);
    /* the sender is reusable after a failure, and the failed seq is not reused */
    l.drop_all = 0;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, (const uint8_t *)"y", 1, 2000), 0);
    CT_EQ(l.tx.seq, 1);
    CT_EQ(settle(&l, 1000), HP_RF_TX_OK);
    /* clock wrap-around */
    link_init(&l);
    l.drop_all = 1;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, (const uint8_t *)"x", 1, 0xffffffc0u), 0);
    hp_rf_tx_poll(&l.tx, 0xffffffc0u + 149);
    CT_EQ(l.sent, 1);
    hp_rf_tx_poll(&l.tx, 0xffffffc0u + 150); /* wraps past zero */
    CT_EQ(l.sent, 2);
}

static void test_tx_all_lost_fails(void)
{
    link_t l;
    uint8_t p[80];
    fill(p, 80);
    link_init(&l);
    l.drop_all = 1;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, p, 80, 0), 0);
    CT_EQ(settle(&l, 10000), HP_RF_TX_FAILED);
    CT_EQ(l.sent, 4 * 4); /* 4 frames, 1 send + 3 retries */
    CT_EQ(l.delivered, 0);
    CT_EQ(hp_rf_tx_poll(&l.tx, l.now + 5000), HP_RF_TX_FAILED); /* stays failed */
    CT_EQ(l.sent, 16);
}

static void test_tx_lost_first_and_continuation(void)
{
    for (int lost = 0; lost < 4; lost++) {
        link_t l;
        uint8_t p[80];
        fill(p, 80);
        link_init(&l);
        l.drop_data[l.ndrop++] = lost; /* one frame of the first transmission */
        CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, p, 80, 0), 0);
        CT_EQ(settle(&l, 10000), HP_RF_TX_OK);
        CT_EQ(l.delivered, 1);
        CT_EQ(l.last_len, 80);
        CT_MEM_EQ(l.last_body, p, 80);
        CT_EQ(l.sent, 8); /* whole object retried */
    }
}

/* The critical case: the ack is lost, the sender retries, the receiver re-acks and delivers once. */
static void test_tx_lost_ack_delivers_once(void)
{
    for (int lost = 1; lost <= 3; lost++)
        for (size_t len = 0; len <= 60; len += 20) {
            link_t l;
            uint8_t p[60];
            fill(p, 60);
            link_init(&l);
            l.drop_acks = lost;
            CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, p, len, 0), 0);
            CT_EQ(settle(&l, 10000), HP_RF_TX_OK);
            CT_EQ(l.delivered, 1);
            CT_EQ(l.acks_sent, lost + 1);
            CT_EQ(l.last_len, len);
            CT_MEM_EQ(l.last_body, p, len);
        }
    /* every ack lost: sender gives up, but the receiver still got the object exactly once */
    link_t l;
    link_init(&l);
    l.drop_acks = 99;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, (const uint8_t *)"k", 1, 0), 0);
    CT_EQ(settle(&l, 10000), HP_RF_TX_FAILED);
    CT_EQ(l.delivered, 1);
    CT_EQ(l.acks_sent, 4);
}

static void test_tx_wrong_ack_ignored(void)
{
    link_t l;
    link_init(&l);
    l.drop_acks = 99;
    CT_EQ(hp_rf_tx_start(&l.tx, T_KEY, (const uint8_t *)"k", 1, 0), 0);
    CT_EQ(hp_rf_tx_on_ack(&l.tx, T_KEY, 5), 0);
    CT_EQ(hp_rf_tx_on_ack(&l.tx, HP_MSG_BATTERY, 0), 0);
    CT_EQ(l.tx.state, HP_RF_TX_WAIT);
    CT_EQ(hp_rf_tx_on_ack(&l.tx, T_KEY, 0), 1);
    CT_EQ(l.tx.state, HP_RF_TX_OK);
}

static void test_seq_wrap_both_directions(void)
{
    link_t dir[2]; /* hub->remote and remote->hub keep independent counters and receivers */
    link_init(&dir[0]);
    link_init(&dir[1]);
    for (int i = 0; i < 70; i++) {
        for (int d = 0; d < 2; d++) {
            uint8_t body[30];
            size_t len = (size_t)(i % 31);
            fill(body, sizeof body);
            body[0] = (uint8_t)i;
            if (d == 1 && i % 3 == 0) continue; /* directions advance independently */
            if (d == 0 && i % 5 == 0) dir[d].drop_acks = 1; /* wrap while retrying */
            CT_EQ(hp_rf_tx_start(&dir[d].tx, T_KEY, body, len, dir[d].now), 0);
            CT_EQ(settle(&dir[d], 1000), HP_RF_TX_OK);
        }
    }
    CT_EQ(dir[0].delivered, 70);
    CT_EQ(dir[0].last_seq, 69 & 0xf);
    CT_EQ(dir[1].delivered, 46);
    CT_EQ(dir[0].tx.next_seq, 70 & 0xf);
    CT_EQ(dir[1].tx.next_seq, 46 & 0xf);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_framing <frames.json>\n"); return 2; }
    CT_RUN(test_every_length);
    CT_RUN(test_split_limits);
    CT_RUN(test_outer_wrap);
    test_vectors(argv[1]);
    CT_RUN(test_lost_first_frame);
    CT_RUN(test_lost_continuation);
    CT_RUN(test_duplicated_frame);
    CT_RUN(test_reordered_frames);
    CT_RUN(test_corrupted_kind_bits);
    CT_RUN(test_new_first_abandons_partial);
    CT_RUN(test_duplicate_object_reacked_not_redelivered);
    CT_RUN(test_ack_frame);
    CT_RUN(test_tx_clean);
    CT_RUN(test_tx_timing);
    CT_RUN(test_tx_all_lost_fails);
    CT_RUN(test_tx_lost_first_and_continuation);
    CT_RUN(test_tx_lost_ack_delivers_once);
    CT_RUN(test_tx_wrong_ack_ignored);
    CT_RUN(test_seq_wrap_both_directions);
    return CT_DONE();
}
