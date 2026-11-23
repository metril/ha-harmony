#include "hub/net/hublog.h"
#include "hub/engine/irout.h"

#include <stdio.h>
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
    return o->pend_n && w == 0 ? 1 : w;
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
        /* timers full: the remaining copies are dropped, never sent back to back (each send would DRAIN the last) */
        hp_log("hub: IR timer queue full, %u repeat copies dropped\n", (unsigned)b->left);
    }
    b->used = 0;
}

static void start_burst(hp_irout_t *o, const hp_ir_signal_t *sig, uint8_t port, uint8_t extra) {
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
    /* no free slot or timer: the extra copies are dropped, never sent back to back */
    hp_log("hub: IR timer queue full, %u repeat copies dropped\n", (unsigned)extra);
}

/* Sends o->cand now (the frame is already encoded there) and books the line. */
static int emit_cand(hp_irout_t *o, hp_ir_sink *ir, uint8_t port_mask, int key, bool sirc, uint8_t copies) {
    hp_ir_signal_t *sig = &o->cand;
    uint32_t now = hp_sched_now(o->sched);
    o->ir = ir;
    int rc = ir->send(ir->self, port_mask, sig);
    if (rc < 0) return rc;
    uint32_t dur_us = 0;
    for (size_t i = 0; i < sig->once_n; i++) dur_us += sig->us[i];
    o->busy_ms = (dur_us + 999u) / 1000u;
    if (sirc && copies < HP_IROUT_SIRC_COPIES) copies = HP_IROUT_SIRC_COPIES;
    if (copies > 1) {
        start_burst(o, sig, port_mask, (uint8_t)(copies - 1));
        o->busy_ms += (uint32_t)(copies - 1) * period_ms(sig);
    }
    line_busy(o, now, o->busy_ms);
    if (key >= 0) {
        o->cand_key = key;
        o->cand_port = port_mask;
    }
    return 0;
}

static void pend_arm(hp_irout_t *o, uint32_t delay);

static void pend_cb(void *arg, uint32_t now) {
    hp_irout_t *o = arg;
    o->pend_h = 0;
    if (!o->pend_n) return;
    uint32_t w = line_wait(o, now, HP_IROUT_GUARD_MS);
    if (w > 0) { /* a hold repeat or another frame took the line meanwhile */
        pend_arm(o, w);
        return;
    }
    hp_irout_pend_t *p = &o->pend[o->pend_head];
    o->cand_key = -1;
    memcpy(&o->cand, &p->sig, sizeof o->cand);
    int er = emit_cand(o, p->ir, p->port, p->key, p->sirc, p->copies);
    void (*done)(void *, int) = p->done;
    void *dctx = p->dctx;
    o->pend_head = (uint8_t)((o->pend_head + 1) % HP_IROUT_PEND);
    o->pend_n--;
    if (er < 0) hp_log("hub: deferred IR send failed (%d)\n", er);
    if (done) done(dctx, er);
    if (o->pend_n && !o->pend_h) {
        uint32_t nw = line_wait(o, hp_sched_now(o->sched), HP_IROUT_GUARD_MS);
        pend_arm(o, nw ? nw : 1);
    }
}

/* On a full timer queue the waiting sends are dropped (logged, their callbacks get -HP_E_BUSY): sending them back
 * to back would make the sink DRAIN between frames on the loop. */
static void pend_arm(hp_irout_t *o, uint32_t delay) {
    o->pend_h = hp_sched_after(o->sched, delay, pend_cb, o);
    if (o->pend_h) return;
    hp_log("hub: IR timer queue full, %u queued sends dropped\n", (unsigned)o->pend_n);
    while (o->pend_n) {
        hp_irout_pend_t *p = &o->pend[o->pend_head];
        void (*done)(void *, int) = p->done;
        void *dctx = p->dctx;
        o->pend_head = (uint8_t)((o->pend_head + 1) % HP_IROUT_PEND);
        o->pend_n--;
        if (done) done(dctx, -HP_E_BUSY);
    }
}

int hp_irout_send(hp_irout_t *o, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key) {
    return hp_irout_send_n(o, ir, code, port_mask, key, 1);
}

int hp_irout_send_n(hp_irout_t *o, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key, uint8_t copies) {
    int r = hp_irout_send_cb(o, ir, code, port_mask, key, copies, NULL, NULL);
    return r < 0 ? r : 0;
}

int hp_irout_send_cb(hp_irout_t *o, hp_ir_sink *ir, const hp_ir_code_t *code, uint8_t port_mask, int key, uint8_t copies,
                     void (*done)(void *ctx, int rc), void *dctx) {
    if (!o || !ir || !code) return -HP_E_INVAL;
    if (copies < 1) copies = 1;
    if (copies > HP_IROUT_MAX_COPIES) copies = HP_IROUT_MAX_COPIES;
    stop_hold(o);
    o->cand_key = -1;
    uint32_t now = hp_sched_now(o->sched);
    uint32_t w = line_wait(o, now, HP_IROUT_GUARD_MS);
    bool defer = key != HP_IROUT_KEY_NONE && (w > 0 || o->pend_n);
    if (defer && o->pend_n >= HP_IROUT_PEND) {
        hp_log("hub: IR line busy, send dropped (queue full)\n");
        return -HP_E_BUSY;
    }
    uint8_t slot = (uint8_t)((o->pend_head + o->pend_n) % HP_IROUT_PEND);
    hp_ir_signal_t *sig = defer ? &o->pend[slot].sig : &o->cand; /* encode straight into its slot */
    uint8_t t = toggle_for(o, code);
    int rc = hp_ir_encode(code, t, sig);
    if (rc < 0) return rc;
    int k = key < 0 ? -1 : key;
    if (!defer) return emit_cand(o, ir, port_mask, k, is_sirc(code), copies);
    o->pend[slot].done = done;
    o->pend[slot].dctx = dctx;
    o->pend[slot].ir = ir;
    o->pend[slot].port = port_mask;
    o->pend[slot].key = k;
    o->pend[slot].sirc = is_sirc(code);
    o->pend[slot].copies = copies;
    o->pend_n++;
    if (!o->pend_h) pend_arm(o, w ? w : 1);
    return 1;
}

static void tick_cb(void *arg, uint32_t now) {
    (void)now;
    hp_irout_t *o = arg;
    o->tick_h = 0;
    if (o->hold_key < 0 || !o->ir) return;
    uint32_t t = hp_sched_now(o->sched);
    if (!bursts_pending(o) && line_wait(o, t, 0) == 0 && !o->pend_n) { /* else skip this repeat */
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
