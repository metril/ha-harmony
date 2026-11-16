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

int hp_irout_send(hp_irout_t *o, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key) {
    if (!o || !ir || !code) return -HP_E_INVAL;
    stop_hold(o);
    o->cand_key = -1;
    hp_ir_signal_t *sig = &o->cand; /* encode straight into the hold candidate */
    uint8_t t = toggle_for(o, code);
    int rc = hp_ir_encode(code, t, sig);
    if (rc < 0) return rc;
    o->ir = ir;
    rc = ir->send(ir->self, port_mask, sig);
    if (rc < 0) return rc;
    if (is_sirc(code)) start_burst(o, sig, port_mask);
    if (key >= 0) {
        o->cand_key = key;
        o->cand_port = port_mask;
    }
    return 0;
}

static void tick_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_t *o = arg;
    o->tick_h = 0;
    if (o->hold_key < 0 || !o->ir) return;
    if (!bursts_pending(o) && o->ir->send(o->ir->self, o->hold_port, &o->rep) < 0) {
        stop_hold(o);
        return;
    }
    o->tick_h = hp_sched_after(o->sched, o->hold_period_ms, tick_cb, o);
    if (!o->tick_h) stop_hold(o);
}

static void cap_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_t *o = arg;
    o->cap_h = 0;
    stop_hold(o);
}

void hp_irout_hold(hp_irout_t *o, int key) {
    if (!o || key < 0 || o->hold_key >= 0 || o->cand_key != key) return;
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
