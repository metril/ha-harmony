#include "hub/ir/encode.h"

#include <ctype.h>
#include <string.h>

#include "hub/hp_errors.h"

static const char *const k_names[HP_IR_PROTO_COUNT] = {
    "nec", "necext", "samsung32", "rc5", "rc6", "sirc12", "sirc15", "sirc20",
};

const char *hp_ir_proto_name(hp_ir_proto_t p)
{
    return (unsigned)p < HP_IR_PROTO_COUNT ? k_names[p] : NULL;
}

int hp_ir_proto_from_name(const char *name, hp_ir_proto_t *p)
{
    if (!name || !p) return -HP_E_NOENT;
    for (int i = 0; i < HP_IR_PROTO_COUNT; i++)
        if (strcmp(name, k_names[i]) == 0) { *p = (hp_ir_proto_t)i; return 0; }
    return -HP_E_NOENT;
}

/* ---- builder ---------------------------------------------------------- */

typedef struct {
    hp_ir_signal_t *s;
    size_t n;
    int last_mark; /* level of us[n-1]; valid when n > start */
    uint32_t sum;  /* duration of the frame being built */
    size_t start;  /* index where the current frame began */
    int err;
} bld_t;

/* Append a level; same-level neighbours within one frame merge. */
static void put(bld_t *b, int mark, uint32_t d)
{
    if (b->err || d == 0) return;
    if (b->n > b->start && b->last_mark == mark) {
        uint32_t v = (uint32_t)b->s->us[b->n - 1] + d;
        if (v > 0xFFFF) { b->err = -HP_E_TOOBIG; return; }
        b->s->us[b->n - 1] = (uint16_t)v;
    } else {
        if (d > 0xFFFF || b->n >= HP_IR_MAX_DUR) { b->err = -HP_E_TOOBIG; return; }
        if (b->n == b->start && !mark) { b->err = -HP_E_INVAL; return; } /* mark first */
        b->s->us[b->n++] = (uint16_t)d;
        b->last_mark = mark;
    }
    b->sum += d;
}

static void frame_begin(bld_t *b) { b->start = b->n; b->sum = 0; }

/* Pad with a space so the frame spans `period` us, clamped to 65535 (`extra` us of idle that
 * precede the first mark belong to the frame but are not listed). */
static void frame_end(bld_t *b, uint32_t period, uint32_t extra)
{
    if (b->err) return;
    uint32_t gap;
    if (b->sum + extra >= period) { b->err = -HP_E_INVAL; return; }
    gap = period - b->sum - extra;
    /* Durations are uint16: clamp the trailing space (see period_us). */
    if (b->n > b->start && !b->last_mark) { /* merges into the final space */
        uint32_t room = 0xFFFFu - b->s->us[b->n - 1];
        if (gap > room) gap = room;
    }
    put(b, 0, gap > 0xFFFFu ? 0xFFFFu : gap);
    b->s->period_us = period;
}

static void lsb_bits(bld_t *b, uint32_t v, int nbits, uint32_t mark, uint32_t sp0, uint32_t sp1)
{
    for (int i = 0; i < nbits; i++) {
        put(b, 1, mark);
        put(b, 0, (v >> i) & 1u ? sp1 : sp0);
    }
}

/* ---- NEC family ------------------------------------------------------- */
/* Public NEC IR transmission protocol: 38 kHz, 9 ms + 4.5 ms leader, bit mark
 * 562.5 us (562 here), space 562.5 us for 0 / 1687.5 us for 1 (1687 here),
 * LSB first, 108 ms frame period; repeat code = 9 ms + 2.25 ms + one 562.5 us
 * mark. Samsung32 uses 4.5 ms + 4.5 ms leader, 560/560/1690 bits, address byte
 * sent twice, then cmd and ~cmd. */
#define NEC_PERIOD 108000u

static void nec_frame(bld_t *b, uint32_t w0, uint32_t w1)
{
    frame_begin(b);
    put(b, 1, 9000);
    put(b, 0, 4500);
    lsb_bits(b, w0, 16, 562, 562, 1687);
    lsb_bits(b, w1, 16, 562, 562, 1687);
    put(b, 1, 562);
    frame_end(b, NEC_PERIOD, 0);
}

static void nec_repeat(bld_t *b)
{
    frame_begin(b);
    put(b, 1, 9000);
    put(b, 0, 2250);
    put(b, 1, 562);
    frame_end(b, NEC_PERIOD, 0);
}

static void samsung_frame(bld_t *b, uint32_t addr, uint32_t cmd)
{
    frame_begin(b);
    put(b, 1, 4500);
    put(b, 0, 4500);
    lsb_bits(b, addr | (addr << 8), 16, 560, 560, 1690);
    lsb_bits(b, cmd | ((~cmd & 0xFFu) << 8), 16, 560, 560, 1690);
    put(b, 1, 560);
    frame_end(b, NEC_PERIOD, 0);
}

/* ---- Manchester families ---------------------------------------------- */
/* RC5: 36 kHz, 889 us half-bit, 14 bits MSB first: S1=1, S2=~cmd6, T, addr5,
 * cmd6. Bit 1 = space then mark, 0 = mark then space. Period 64 bit times
 * (113.792 ms). The idle half-bit before the first mark is not listed but is
 * counted in the period. */
#define RC5_HALF 889u
#define RC5_PERIOD (64u * 2u * RC5_HALF)

static void rc5_frame(bld_t *b, uint32_t addr, uint32_t cmd, int t)
{
    uint32_t w = (1u << 13) | ((cmd & 0x40u ? 0u : 1u) << 12) | ((uint32_t)t << 11) |
                 ((addr & 0x1Fu) << 6) | (cmd & 0x3Fu);
    frame_begin(b);
    for (int i = 13; i >= 0; i--) {
        int bit = (int)((w >> i) & 1u);
        if (i == 13) { put(b, 1, RC5_HALF); continue; } /* S1: idle half skipped */
        put(b, bit ? 0 : 1, RC5_HALF);
        put(b, bit ? 1 : 0, RC5_HALF);
    }
    frame_end(b, RC5_PERIOD, RC5_HALF);
}

/* RC6 mode 0: 36 kHz, t = 444 us (16 carrier cycles). Leader mark 6t, space
 * 2t; start bit 1; mode 000; trailer (toggle) bit of width 2t; addr8; cmd8;
 * MSB first. Bit 1 = mark then space, 0 = space then mark. Period 107 ms. */
#define RC6_T 444u
#define RC6_PERIOD 107000u

static void rc6_bit(bld_t *b, int bit, uint32_t t)
{
    put(b, bit ? 1 : 0, t);
    put(b, bit ? 0 : 1, t);
}

static void rc6_frame(bld_t *b, uint32_t addr, uint32_t cmd, int t)
{
    frame_begin(b);
    put(b, 1, 6 * RC6_T);
    put(b, 0, 2 * RC6_T);
    rc6_bit(b, 1, RC6_T); /* start */
    for (int i = 0; i < 3; i++) rc6_bit(b, 0, RC6_T); /* mode 0 */
    rc6_bit(b, t, 2 * RC6_T);
    for (int i = 7; i >= 0; i--) rc6_bit(b, (int)((addr >> i) & 1u), RC6_T);
    for (int i = 7; i >= 0; i--) rc6_bit(b, (int)((cmd >> i) & 1u), RC6_T);
    frame_end(b, RC6_PERIOD, 0);
}

/* ---- Sony SIRC -------------------------------------------------------- */
/* 40 kHz; header mark 2400 us + space 600 us; bit = mark 600 (0) / 1200 (1)
 * + space 600; LSB first: 7 cmd bits then address (5 bits SIRC12, 8 bits
 * SIRC15, 13 bits SIRC20 = 5-bit address then 8-bit extension as one field).
 * Frame period 45 ms start to start. */
#define SIRC_PERIOD 45000u

static void sirc_frame(bld_t *b, uint32_t addr, uint32_t cmd, int abits)
{
    frame_begin(b);
    put(b, 1, 2400);
    put(b, 0, 600);
    for (int i = 0; i < 7 + abits; i++) {
        uint32_t v = i < 7 ? cmd >> i : addr >> (i - 7);
        put(b, 1, v & 1u ? 1200 : 600);
        put(b, 0, 600);
    }
    frame_end(b, SIRC_PERIOD, 0);
}

/* ---- dispatch --------------------------------------------------------- */

static int encode_proto(const hp_ir_code_t *c, uint8_t toggle, hp_ir_signal_t *o)
{
    hp_ir_proto_t p = c->u.p.proto;
    uint32_t a = c->u.p.addr, m = c->u.p.cmd;
    int t = toggle ? 1 : 0;
    uint32_t amax, cmax;
    int abits = 0;
    switch (p) {
    case HP_IR_NEC: case HP_IR_SAMSUNG32: case HP_IR_RC6: amax = 0xFF; cmax = 0xFF; break;
    case HP_IR_NECEXT: amax = 0xFFFF; cmax = 0xFF; break;
    case HP_IR_RC5: amax = 0x1F; cmax = 0x7F; break;
    case HP_IR_SIRC12: amax = 0x1F; cmax = 0x7F; abits = 5; break;
    case HP_IR_SIRC15: amax = 0xFF; cmax = 0x7F; abits = 8; break;
    case HP_IR_SIRC20: amax = 0x1FFF; cmax = 0x7F; abits = 13; break;
    default: return -HP_E_INVAL;
    }
    if (a > amax || m > cmax) return -HP_E_INVAL;

    bld_t b = {.s = o};
    if (p == HP_IR_NEC || p == HP_IR_NECEXT) {
        o->carrier_hz = 38000;
        nec_frame(&b, p == HP_IR_NEC ? (a | ((~a & 0xFFu) << 8)) : a, m | ((~m & 0xFFu) << 8));
        o->once_n = (uint16_t)b.n;
        nec_repeat(&b);
    } else {
        for (int pass = 0; pass < 2 && !b.err; pass++) {
            switch (p) {
            case HP_IR_SAMSUNG32: o->carrier_hz = 38000; samsung_frame(&b, a, m); break;
            case HP_IR_RC5: o->carrier_hz = 36000; rc5_frame(&b, a, m, t); break;
            case HP_IR_RC6: o->carrier_hz = 36000; rc6_frame(&b, a, m, t); break;
            default: o->carrier_hz = 40000; sirc_frame(&b, a, m, abits); break;
            }
            if (pass == 0) o->once_n = (uint16_t)b.n;
        }
    }
    if (b.err) return b.err;
    o->rep_n = (uint16_t)(b.n - o->once_n);
    return 0;
}

static int encode_raw(const hp_ir_code_t *c, hp_ir_signal_t *o)
{
    uint32_t hz = c->u.raw.hz;
    size_t n = c->u.raw.n;
    if (hz < 1000 || hz > 1000000 || !c->u.raw.us || n == 0 || (n & 1)) return -HP_E_INVAL;
    if (n > HP_IR_MAX_DUR) return -HP_E_TOOBIG;
    for (size_t i = 0; i < n; i++) {
        if (c->u.raw.us[i] == 0) return -HP_E_INVAL;
        if (c->u.raw.us[i] > 0xFFFF) return -HP_E_TOOBIG;
    }
    for (size_t i = 0; i < n; i++) o->us[i] = (uint16_t)c->u.raw.us[i];
    o->carrier_hz = hz;
    o->once_n = (uint16_t)n;
    o->rep_n = 0;
    return 0;
}

/* Pronto Hex "learned" format 0000: word1 = frequency code (carrier =
 * 1e6 / (code * 0.241246) Hz), word2/3 = once/repeat burst-pair counts; each
 * burst word counts carrier periods. */
static int encode_pronto(const hp_ir_code_t *c, hp_ir_signal_t *o)
{
    const uint16_t *w = c->u.pronto.words;
    size_t n = c->u.pronto.n;
    if (!w || n < 4 || w[0] != 0 || w[1] == 0) return -HP_E_INVAL;
    size_t once = w[2], rep = w[3];
    if (once + rep == 0 || n != 4 + 2 * (once + rep)) return -HP_E_INVAL;
    if (2 * (once + rep) > HP_IR_MAX_DUR) return -HP_E_TOOBIG;
    uint64_t per = (uint64_t)w[1] * 241246u; /* carrier period in 1e-6 us */
    for (size_t i = 0; i < 2 * (once + rep); i++) {
        uint64_t d = ((uint64_t)w[4 + i] * per + 500000u) / 1000000u;
        if (d == 0) return -HP_E_INVAL;
        if (d > 0xFFFF) return -HP_E_TOOBIG;
        o->us[i] = (uint16_t)d;
    }
    o->carrier_hz = (uint32_t)((1000000000000ull + per / 2) / per);
    o->once_n = (uint16_t)(2 * once);
    o->rep_n = (uint16_t)(2 * rep);
    return 0;
}

int hp_ir_encode(const hp_ir_code_t *code, uint8_t toggle, hp_ir_signal_t *out)
{
    int r;
    if (!code || !out) return -HP_E_INVAL;
    memset(out, 0, sizeof *out);
    switch (code->kind) {
    case HP_IR_CODE_PROTO: r = encode_proto(code, toggle, out); break;
    case HP_IR_CODE_RAW: r = encode_raw(code, out); break;
    case HP_IR_CODE_PRONTO: r = encode_pronto(code, out); break;
    default: r = -HP_E_INVAL; break;
    }
    if (r) memset(out, 0, sizeof *out);
    return r;
}

int hp_ir_parse_pronto(const char *s, uint16_t *words, size_t cap, size_t *n)
{
    size_t cnt = 0;
    if (!s || !words || !n) return -HP_E_INVAL;
    while (*s) {
        uint32_t v = 0;
        int digits = 0;
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        while (isxdigit((unsigned char)*s)) {
            char ch = *s++;
            v = v * 16 + (uint32_t)(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10);
            if (++digits > 4) return -HP_E_INVAL;
        }
        if (!digits || (*s && !isspace((unsigned char)*s))) return -HP_E_INVAL;
        if (cnt >= cap) return -HP_E_TOOBIG;
        words[cnt++] = (uint16_t)v;
    }
    if (!cnt) return -HP_E_INVAL;
    *n = cnt;
    return 0;
}
