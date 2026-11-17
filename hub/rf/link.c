#include "hub/rf/link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hub/hp_errors.h"

void hp_rf_link_init(hp_rf_link_t *l, hp_rf_transport *tp, bool wrapped, uint8_t slot,
                     hp_rf_link_deliver_fn deliver, void *ctx) {
    memset(l, 0, sizeof *l);
    l->tp = tp;
    l->wrapped = wrapped;
    l->slot = slot;
    l->deliver = deliver;
    l->deliver_ctx = ctx;
    l->gap_ms = HP_LINK_GAP_MS;
    l->tx_slot = HP_LINK_SLOT_ANY;
    hp_rf_asm_init(&l->as);
}

void hp_rf_log_stderr(void *ctx, const char *line) {
    (void)ctx;
    fprintf(stderr, "%s\n", line);
}

void hp_rf_link_set_log(hp_rf_link_t *l, hp_rf_link_log_fn fn, void *ctx) {
    l->log = fn;
    l->log_ctx = ctx;
}

static void lg(hp_rf_link_t *l, uint32_t now, const char *fmt, ...) {
    if (!l->log) return;
    char line[160];
    int n = snprintf(line, sizeof line, "link %8u ms: ", (unsigned)now);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    l->log(l->log_ctx, line);
}

static bool strict_on(const hp_rf_link_t *l) { return l->strict && l->tp->tx_status; }
static bool link_up(const hp_rf_link_t *l) { return !l->tp->attached || l->tp->attached(l->tp->self); }

static int write_raw(hp_rf_link_t *l, const hp_rf_inner_t *fr) {
    int rc;
    if (l->wrapped) {
        hp_rf_frame_t w;
        hp_rf_wrap_hub(l->slot, fr, &w);
        rc = l->tp->send(l->tp->self, w.data, w.len);
    } else {
        rc = l->tp->send(l->tp->self, fr->b, HP_RF_INNER_LEN);
    }
    l->last_rc = rc;
    l->last_tx = l->now_dbg;
    return rc;
}

/* Strict mode only buffers: flush_out() owns the radio. */
static int emit_raw(hp_rf_link_t *l, const hp_rf_inner_t *fr, bool is_ack) {
    if (!strict_on(l)) return write_raw(l, fr);
    if (is_ack) {
        l->pa = *fr;
        l->pa_v = true;
    } else {
        l->pd = *fr;
        l->pd_v = true;
    }
    l->last_rc = 0;
    return 0;
}

/* The sender's frames: a TX status only counts for a frame handed over after the previous one. */
static int emit(void *vl, const hp_rf_inner_t *fr) {
    hp_rf_link_t *l = vl;
    l->released = false;
    int rc = emit_raw(l, fr, false);
    lg(l, l->now_dbg, "frame %zu/%zu handed to transport (seq %u) rc=%d", l->tx.sent ? l->tx.sent : 1, l->tx.n,
       (unsigned)l->tx.seq, rc);
    return rc;
}

/* The frame in the radio is finished: delivered (status 00) or not (01 / link loss: kept for a rewrite). */
static void out_done(hp_rf_link_t *l, bool delivered) {
    if (!l->out_busy) return;
    l->out_busy = false;
    if (delivered) {
        if (!l->out_ack) l->released = true;
    } else if (l->out_ack) {
        if (!l->pa_v) { l->pa = l->out_fr; l->pa_v = true; }
    } else if (!l->pd_v) {
        l->pd = l->out_fr;
        l->pd_v = true;
    }
}

/* Writes the next downlink frame when the radio is free and the link is up. Acks first. */
static void flush_out(hp_rf_link_t *l, uint32_t now) {
    if (!strict_on(l)) return;
    if (l->out_busy && (int32_t)(now - l->out_at) >= HP_LINK_OUT_TIMEOUT_MS) {
        lg(l, now, "no TX status for the frame in the radio: taking it as done");
        out_done(l, true);
    }
    if (l->out_busy || !link_up(l)) return;
    bool ack = l->pa_v;
    if (!ack && !l->pd_v) return;
    const hp_rf_inner_t *fr = ack ? &l->pa : &l->pd;
    if (write_raw(l, fr) < 0) return; /* kept; tried again next pump (the transport backs off) */
    l->out_busy = true;
    l->out_ack = ack;
    l->out_fr = *fr;
    l->out_at = now;
    if (ack) l->pa_v = false;
    else {
        l->pd_v = false;
        l->released = false;
    }
    lg(l, now, "wrote %s frame to the radio", ack ? "ack" : "data");
}

static void reset(hp_rf_link_t *l) {
    if (strict_on(l)) { /* the link dropped: keep what is queued (it is sent after the next link-up) */
        l->as.active = 0;
        out_done(l, false);
        return;
    }
    hp_rf_asm_init(&l->as);
    l->failures += (uint32_t)l->qn;
    l->qhead = l->qn = 0;
    l->started = false;
    l->start_tries = 0;
    uint8_t seq = l->tx.next_seq; /* keep counting: the peer may not have reset its duplicate filter */
    hp_rf_tx_init(&l->tx, emit, l);
    l->tx.next_seq = seq;
}

static void pop(hp_rf_link_t *l) {
    l->qhead = (l->qhead + 1) % HP_LINK_QUEUE;
    l->qn--;
    l->started = false;
    l->pd_v = false; /* a stale frame of the finished object */
}

static void drive_tx(hp_rf_link_t *l, uint32_t now);

int hp_rf_link_send(hp_rf_link_t *l, hp_msg_type_t type, const uint8_t *body, size_t len, uint32_t now_ms) {
    if (!l || !l->tp || (len && !body) || (unsigned)type > 0xFF) return -HP_E_INVAL;
    if (len > HP_RF_BODY_MAX) return -HP_E_TOOBIG;
    l->now_dbg = now_ms;
    if (l->tp->attached && !l->tp->attached(l->tp->self)) return -HP_E_NOENT;
    if (l->qn == HP_LINK_QUEUE) return -HP_E_BUSY;
    if (!l->tx.send) hp_rf_tx_init(&l->tx, emit, l);
    hp_rf_link_obj_t *o = &l->q[(l->qhead + l->qn) % HP_LINK_QUEUE];
    o->type = type;
    o->len = len;
    if (len) memcpy(o->body, body, len);
    l->qn++;
    if (l->qn == 1) { /* nothing ahead of it: transmit now, so "no peer" is reported synchronously */
        l->noent = false;
        drive_tx(l, now_ms);
        if (l->noent) {
            l->noent = false;
            return -HP_E_NOENT;
        }
    }
    return 0;
}

static void send_ack(hp_rf_link_t *l, const hp_rf_inner_t *ack) { (void)emit_raw(l, ack, true); }

static void on_frame(hp_rf_link_t *l, const uint8_t *f, size_t n) {
    hp_rf_inner_t in, ack;
    hp_rf_msg_t msg;
    if (l->wrapped) {
        uint8_t slot;
        if (hp_rf_unwrap_hub(f, n, &slot, &in) < 0) return;
    } else {
        if (n != HP_RF_INNER_LEN) return;
        memcpy(in.b, f, HP_RF_INNER_LEN);
    }
    l->rx_frames++;
    switch (hp_rf_asm_push(&l->as, &in, &msg, &ack)) {
    case HP_ASM_COMPLETE:
        l->delivered++;
        lg(l, l->now_dbg, "rx object type %d seq %u len %zu delivered", (int)msg.type, (unsigned)msg.seq, msg.len);
        send_ack(l, &ack);
        if (l->deliver) l->deliver(l->deliver_ctx, &msg);
        break;
    case HP_ASM_DUPLICATE:
        lg(l, l->now_dbg, "rx duplicate seq %u (ack only)", (unsigned)ack.b[0] & 0x0F);
        send_ack(l, &ack);
        break;
    case HP_ASM_ACK: {
        int hit = hp_rf_tx_on_ack(&l->tx, (uint8_t)msg.type, msg.seq);
        lg(l, l->now_dbg, "rx ack type %d seq %u%s", (int)msg.type, (unsigned)msg.seq, hit ? " (completes in-flight object)" : " (ignored)");
        break;
    }
    default: break; /* HP_ASM_POLL and garbage: ignored */
    }
}

static void drive_tx(hp_rf_link_t *l, uint32_t now) {
    for (int guard = 0; guard < HP_LINK_QUEUE + 1 && l->qn; guard++) {
        hp_rf_link_obj_t *o = &l->q[l->qhead];
        if (!l->started) {
            if (l->start_tries && (int32_t)(now - l->start_at) < 0) return;
            l->tx.pace_ms = l->gap_ms;
            lg(l, now, "send start type %d len %zu seq %u", (int)o->type, o->len, (unsigned)(l->tx.next_seq & 0x0F));
            int rc = hp_rf_tx_start(&l->tx, o->type, o->body, o->len, now);
            if (rc == -HP_E_IO && l->last_rc == -HP_E_NOENT) { /* nobody there: not a retryable failure */
                l->start_tries = 0;
                l->noent = true;
                pop(l);
                continue;
            }
            if (rc == -HP_E_IO) { /* transport refused the first frame (backoff, chip busy): retry later */
                if (++l->start_tries > HP_RF_RETRIES) {
                    l->failures++;
                    l->start_tries = 0;
                    pop(l);
                    continue;
                }
                l->start_at = now + HP_RF_RETRY_MS;
                return;
            }
            l->start_tries = 0;
            if (rc < 0) { /* unsendable object: drop it */
                l->failures++;
                pop(l);
                continue;
            }
            l->started = true;
            return;
        }
        bool hold = strict_on(l) && (l->out_busy || l->pd_v || !link_up(l));
        if (strict_on(l)) {
            if (!hold) (void)hp_rf_tx_service(&l->tx, now, 1); /* radio free: the next frame of the object */
        } else {
            (void)hp_rf_tx_service(&l->tx, now, l->released);
        }
        uint8_t was = l->tx.retries;
        hp_rf_tx_state_t st;
        if (hold && l->tx.state == HP_RF_TX_WAIT) { /* no retry clock while a frame is in the radio or the link is down */
            l->tx.deadline = now + HP_RF_RETRY_MS;
            st = HP_RF_TX_WAIT;
        } else {
            st = hp_rf_tx_poll(&l->tx, now);
        }
        if (l->tx.retries != was) lg(l, now, "retry %u of %d (seq %u)", (unsigned)l->tx.retries, HP_RF_RETRIES, (unsigned)l->tx.seq);
        switch (st) {
        case HP_RF_TX_OK: lg(l, now, "tx done: object acked (seq %u)", (unsigned)l->tx.seq); pop(l); continue;
        case HP_RF_TX_FAILED: lg(l, now, "tx FAILED after retries (seq %u)", (unsigned)l->tx.seq); l->failures++; pop(l); continue;
        default: return;
        }
    }
}

static uint32_t poll_iv(const hp_rf_link_t *l) {
    bool waiting = l->qn && l->started && l->tx.state == HP_RF_TX_WAIT;
    return waiting || !l->idle_poll_ms ? l->poll_ms : l->idle_poll_ms;
}

/* ms until the next poll is due (0 = now), -1 when not polling. */
static int poll_due(const hp_rf_link_t *l, uint32_t now) {
    if (!l->poll_ms || !l->awake) return -1;
    int32_t d = (int32_t)(l->last_tx + poll_iv(l) - now);
    return d < 0 ? 0 : d;
}

void hp_rf_link_set_poll(hp_rf_link_t *l, uint32_t poll_ms, uint32_t idle_poll_ms) {
    l->poll_ms = poll_ms;
    l->idle_poll_ms = idle_poll_ms;
}

void hp_rf_link_set_awake(hp_rf_link_t *l, bool awake, uint32_t now_ms) {
    if (awake && !l->awake) l->last_tx = now_ms - (l->idle_poll_ms > l->poll_ms ? l->idle_poll_ms : l->poll_ms);
    l->awake = awake;
}

int hp_rf_link_pump(hp_rf_link_t *l, uint32_t now_ms) {
    uint8_t fr[HP_RF_FRAME_MAX + 1];
    int rc = 0, n;
    l->now_dbg = now_ms;
    if (!l->tx.send) hp_rf_tx_init(&l->tx, emit, l);
    while ((n = l->tp->recv(l->tp->self, fr, sizeof fr, 0)) != 0) {
        if (n < 0) {
            if (n == -HP_E_NOENT) reset(l);
            rc = n;
            break;
        }
        on_frame(l, fr, (size_t)n);
    }
    if (l->tp->tx_status) {
        uint8_t sl, st;
        while (l->tp->tx_status(l->tp->self, &sl, &st)) {
            bool mine = l->tx_slot == HP_LINK_SLOT_ANY || sl == l->tx_slot;
            lg(l, now_ms, "tx status slot %u status 0x%02x%s", (unsigned)sl, (unsigned)st, mine ? "" : " (other slot, ignored)");
            if (!mine) continue;
            if (strict_on(l)) out_done(l, st == 0x00); /* a status with nothing in the radio (e.g. at link-up) is ignored */
            else l->released = true;
        }
    }
    drive_tx(l, now_ms);
    if (l->noent) { /* a queued object found nobody home */
        l->noent = false;
        l->failures++;
    }
    flush_out(l, now_ms);
    if (poll_due(l, now_ms) == 0) {
        hp_rf_inner_t p;
        hp_rf_make_poll(&p);
        (void)write_raw(l, &p); /* best effort; not logged (the transport's own debug shows it) */
    }
    return rc;
}

int hp_rf_link_next_timeout_ms(const hp_rf_link_t *l, uint32_t now_ms) {
    int best = poll_due(l, now_ms);
    if (strict_on(l) && l->out_busy) {
        int32_t d = (int32_t)(l->out_at + HP_LINK_OUT_TIMEOUT_MS - now_ms);
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    if (!l->qn) return best;
    uint32_t at = l->started ? l->tx.deadline : l->start_tries ? l->start_at : now_ms;
    if (l->started && l->tx.state == HP_RF_TX_WAIT && l->tx.sent < l->tx.n) at = l->tx.next_at; /* pacing */
    int32_t d = (int32_t)(at - now_ms);
    if (d < 0) d = 0;
    return best < 0 || d < best ? d : best;
}

hp_rf_link_status_t hp_rf_link_status(const hp_rf_link_t *l) {
    return (hp_rf_link_status_t){l->qn, l->failures, l->delivered};
}
