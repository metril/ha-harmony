#include "hub/engine/irout.h"

#include <string.h>

#include "hub/hp_errors.h"

void hp_irout_init(hp_irout_t *o, hp_sched_t *s) {
    memset(o, 0, sizeof *o);
    o->sched = s;
    o->cand_key = -1;
    o->hold_key = -1;
}

static bool is_sirc(const hp_ir_code_t *c) {
    return c->kind == HP_IR_CODE_PROTO &&
           (c->u.p.proto == HP_IR_SIRC12 || c->u.p.proto == HP_IR_SIRC15 || c->u.p.proto == HP_IR_SIRC20);
}

static uint32_t period_ms(const hp_ir_signal_t *s) {
    uint32_t ms = (s->period_us + 999u) / 1000u;
    return ms ? ms : HP_IROUT_DEFAULT_PERIOD_MS;
}

static uint8_t toggle_for(hp_irout_t *o, const hp_ir_code_t *c) {
    if (c->kind != HP_IR_CODE_PROTO || (c->u.p.proto != HP_IR_RC5 && c->u.p.proto != HP_IR_RC6)) return 0;
    for (int i = 0; i < HP_IROUT_TOGGLES; i++) {
        if (o->tog[i].used && o->tog[i].proto == c->u.p.proto && o->tog[i].addr == c->u.p.addr) {
            o->tog[i].t ^= 1u;
            return o->tog[i].t;
        }
    }
    int i = o->tog_next++ % HP_IROUT_TOGGLES;
    o->tog[i].used = 1;
    o->tog[i].proto = (uint8_t)c->u.p.proto;
    o->tog[i].addr = c->u.p.addr;
    o->tog[i].t = 0;
    return 0;
}

static void stop_hold(hp_irout_t *o) {
    if (o->hold_key < 0) return;
    hp_sched_cancel(o->sched, o->tick_h);
    hp_sched_cancel(o->sched, o->cap_h);
    o->tick_h = o->cap_h = 0;
    o->hold_key = -1;
}

/* Ms until the line is free, `guard` ms after the last frame ended. */
static uint32_t line_wait(const hp_irout_t *o, uint32_t now, uint32_t guard) {
    uint32_t d = o->line_free + guard - now; /* unsigned: bounded by the span it was set with */
    return d > 0 && d <= o->line_len + guard ? d : 0;
}

static void line_busy(hp_irout_t *o, uint32_t now, uint32_t len) {
    o->line_len = len;
    o->line_free = now + len;
}

uint32_t hp_irout_wait_ms(const hp_irout_t *o, uint32_t now) {
    uint32_t w = line_wait(o, now, HP_IROUT_GUARD_MS);
    return o->pend && w == 0 ? 1 : w;
}

static bool bursts_pending(const hp_irout_t *o) {
    for (int i = 0; i < HP_IROUT_BURSTS; i++)
        if (o->burst[i].used) return true;
    return false;
}

static void burst_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_burst_t *b = arg;
    hp_irout_t *o = b->o;
    b->h = 0;
    if (o->ir) (void)o->ir->send(o->ir->self, b->port, &b->sig);
    if (--b->left > 0) {
        b->h = hp_sched_after(o->sched, b->period_ms, burst_cb, b);
        if (b->h) return;
        while (b->left-- > 0 && o->ir) (void)o->ir->send(o->ir->self, b->port, &b->sig); /* timers full */
    }
    b->used = 0;
}

static void start_burst(hp_irout_t *o, const hp_ir_signal_t *sig, uint8_t port) {
    uint8_t extra = HP_IROUT_SIRC_COPIES - 1;
    uint32_t ms = period_ms(sig);
    for (int i = 0; i < HP_IROUT_BURSTS; i++) {
        hp_irout_burst_t *b = &o->burst[i];
        if (b->used) continue;
        b->used = 1;
        b->o = o;
        b->left = extra;
        b->port = port;
        b->period_ms = ms;
        b->sig = *sig;
        b->h = hp_sched_after(o->sched, ms, burst_cb, b);
        if (b->h) return;
        b->used = 0;
        break;
    }
    /* no free slot or timer: degrade to back-to-back frames (the sink paces them) */
    while (extra--) (void)o->ir->send(o->ir->self, port, sig);
}

/* Sends o->cand now (the frame is already encoded there) and books the line. */
static int emit_cand(hp_irout_t *o, hp_ir_sink *ir, uint8_t port_mask, int key, bool sirc) {
    hp_ir_signal_t *sig = &o->cand;
    uint32_t now = hp_sched_now(o->sched);
    o->ir = ir;
    int rc = ir->send(ir->self, port_mask, sig);
    if (rc < 0) return rc;
    uint32_t dur_us = 0;
    for (size_t i = 0; i < sig->once_n; i++) dur_us += sig->us[i];
    o->busy_ms = (dur_us + 999u) / 1000u;
    if (sirc) {
        start_burst(o, sig, port_mask);
        o->busy_ms += (uint32_t)(HP_IROUT_SIRC_COPIES - 1) * period_ms(sig);
    }
    line_busy(o, now, o->busy_ms);
    if (key >= 0) {
        o->cand_key = key;
        o->cand_port = port_mask;
    }
    return 0;
}

static void pend_cb(void *arg, uint32_t now) {
    hp_irout_t *o = arg;
    o->pend_h = 0;
    if (!o->pend) return;
    uint32_t w = line_wait(o, now, HP_IROUT_GUARD_MS);
    if (w > 0) { /* a hold repeat or another frame took the line meanwhile */
        o->pend_h = hp_sched_after(o->sched, w, pend_cb, o);
        if (o->pend_h) return;
    }
    o->pend = false;
    o->cand_key = -1;
    memcpy(&o->cand, &o->pend_sig, sizeof o->cand);
    (void)emit_cand(o, o->pend_ir, o->pend_port, o->pend_key, o->pend_sirc);
}

int hp_irout_send(hp_irout_t *o, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key) {
    if (!o || !ir || !code) return -HP_E_INVAL;
    stop_hold(o);
    o->cand_key = -1;
    uint32_t now = hp_sched_now(o->sched);
    uint32_t w = line_wait(o, now, HP_IROUT_GUARD_MS);
    bool defer = key != HP_IROUT_KEY_NONE && (w > 0 || o->pend);
    hp_ir_signal_t *sig = defer ? &o->pend_sig : &o->cand; /* encode straight into its slot */
    uint8_t t = toggle_for(o, code);
    int rc = hp_ir_encode(code, t, sig);
    if (rc < 0) return rc;
    if (!defer) return emit_cand(o, ir, port_mask, key < 0 ? -1 : key, is_sirc(code));
    hp_sched_cancel(o->sched, o->pend_h); /* newest wins: the older deferred send is dropped */
    o->pend = true;
    o->pend_ir = ir;
    o->pend_port = port_mask;
    o->pend_key = key < 0 ? -1 : key;
    o->pend_sirc = is_sirc(code);
    o->pend_h = hp_sched_after(o->sched, w ? w : 1, pend_cb, o);
    if (!o->pend_h) { /* no timer: degrade to sending now */
        o->pend = false;
        memcpy(&o->cand, &o->pend_sig, sizeof o->cand);
        return emit_cand(o, ir, port_mask, key < 0 ? -1 : key, o->pend_sirc);
    }
    return 0;
}

static void tick_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_t *o = arg;
    o->tick_h = 0;
    if (o->hold_key < 0 || !o->ir) return;
    uint32_t t = hp_sched_now(o->sched);
    if (!bursts_pending(o) && line_wait(o, t, 0) == 0 && !o->pend) { /* else skip this repeat */
        if (o->ir->send(o->ir->self, o->hold_port, &o->rep) < 0) {
            stop_hold(o);
            return;
        }
        uint32_t us = 0;
        for (size_t i = 0; i < o->rep.once_n; i++) us += o->rep.us[i];
        line_busy(o, t, (us + 999u) / 1000u);
    }
    o->tick_h = hp_sched_after(o->sched, o->hold_period_ms, tick_cb, o);
    if (!o->tick_h) stop_hold(o);
}

static void cap_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_t *o = arg;
    o->cap_h = 0;
    stop_hold(o);
    o->cand_key = -1; /* the key is considered released: a late HOLD must not resume the repeats */
}

void hp_irout_hold(hp_irout_t *o, int key) {
    if (!o || key < 0) return;
    if (o->hold_key == key) { /* another HOLD of the same key: the cap restarts from here */
        hp_sched_cancel(o->sched, o->cap_h);
        o->cap_h = hp_sched_after(o->sched, HP_IROUT_HOLD_CAP_MS, cap_cb, o);
        if (!o->cap_h) stop_hold(o);
        return;
    }
    if (o->hold_key >= 0 || o->cand_key != key) return;
    const hp_ir_signal_t *c = &o->cand;
    o->rep = *c;
    if (c->rep_n > 0) { /* repeat sequence: shift it to the front */
        memmove(o->rep.us, c->us + c->once_n, (size_t)c->rep_n * sizeof c->us[0]);
        o->rep.once_n = c->rep_n;
    }
    o->rep.rep_n = 0;
    o->hold_port = o->cand_port;
    o->hold_period_ms = period_ms(c);
    o->hold_key = key;
    o->tick_h = hp_sched_after(o->sched, o->hold_period_ms, tick_cb, o);
    o->cap_h = hp_sched_after(o->sched, HP_IROUT_HOLD_CAP_MS, cap_cb, o);
    if (!o->tick_h || !o->cap_h) stop_hold(o);
}

void hp_irout_release(hp_irout_t *o, int key) {
    if (!o) return;
    if (key >= 0 && key != o->hold_key && key != o->cand_key) return;
    stop_hold(o);
    o->cand_key = -1;
}

void hp_irout_cancel(hp_irout_t *o) {
    if (!o) return;
    stop_hold(o);
    o->cand_key = -1;
}
