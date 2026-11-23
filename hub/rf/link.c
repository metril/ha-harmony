#include "hub/rf/link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hub/hp_errors.h"
#include "hub/rf/rf_payload.h"

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

typedef enum { REL_ST00, REL_ST01, REL_UPLINK, REL_TIMEOUT, REL_LOSS } rel_t;

/* The frame in the radio is finished: delivered (status 00, an uplink frame after it, the timeout) or not
 * (01 / link loss: kept for a rewrite). */
static void out_done(hp_rf_link_t *l, bool delivered, rel_t why) {
    if (!l->out_busy) return;
    static const char *const names[] = {"status 00", "status 01", "uplink", "timeout", "link loss"};
    lg(l, l->now_dbg, "radio frame (%s) released by %s after %u ms", l->out_ack ? "ack" : "data", names[why],
       (unsigned)(l->now_dbg - l->out_at));
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
        out_done(l, true, REL_TIMEOUT);
    }
    if (l->out_busy || !link_up(l)) return;
    bool ack = l->pa_v;
    if (!ack && !l->pd_v) return;
    const hp_rf_inner_t *fr = ack ? &l->pa : &l->pd;
    if (write_raw(l, fr) < 0) { /* kept; tried again when the transport's backoff ends */
        l->wr_retry_at = now + HP_LINK_WRITE_RETRY_MS;
        return;
    }
    l->out_busy = true;
    l->out_ack = ack;
    l->out_fr = *fr;
    l->out_at = now;
    uint32_t rx_at;
    l->out_wr_ok = l->tp->frame_times && l->tp->frame_times(l->tp->self, &rx_at, &l->out_wr);
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
        out_done(l, false, REL_LOSS);
        return;
    }
    /* The queue is kept too (a loss is not a send failure). The retry clock is held while the link is down and stale
     * objects are dropped by expire_stale(); the object in flight gets a fresh set of retries after the link-up. */
    hp_rf_asm_init(&l->as);
    if (l->tx.state == HP_RF_TX_WAIT) l->tx.retries = 0;
}

static void pop(hp_rf_link_t *l) {
    l->qhead = (l->qhead + 1) % HP_LINK_QUEUE;
    l->qn--;
    l->started = false;
    l->start_wait = false;
    l->pd_v = false; /* a stale frame of the finished object */
}

static void drive_tx(hp_rf_link_t *l, uint32_t now);
static void report(hp_rf_link_t *l, const hp_rf_link_obj_t *o, bool ok);

static hp_rf_link_obj_t *qat(hp_rf_link_t *l, size_t i) { return &l->q[(l->qhead + i) % HP_LINK_QUEUE]; }

/* A key_event object: its logical key and action. */
static bool key_info(const hp_rf_link_obj_t *o, uint32_t *code, uint32_t *action) {
    hp_rf_val_t v[3];
    if (o->type != HP_MSG_KEY_EVENT || hp_rf_unpack(o->type, o->body, o->len, v, 3) < 0) return false;
    *code = v[1].u;
    *action = v[2].u;
    return true;
}

/* Removes queued object i (never counted as a send failure). */
static void remove_at(hp_rf_link_t *l, size_t i, bool notify) {
    if (notify) report(l, qat(l, i), false);
    if (i == 0) {
        if (l->started) l->tx.state = HP_RF_TX_IDLE;
        l->started = false;
        l->start_wait = false;
        l->start_tries = 0;
        l->pd_v = false;
    }
    for (size_t k = i; k + 1 < l->qn; k++) *qat(l, k) = *qat(l, k + 1);
    l->qn--;
}

/* The newest RELEASE of a key is never dropped: the hub would see the key as held forever. */
static bool keep_release(hp_rf_link_t *l, size_t i, uint32_t now) {
    uint32_t code, act, c2, a2;
    if ((int32_t)(now - qat(l, i)->queued_at) >= HP_LINK_RELEASE_KEEP_MS) return false;
    if (!key_info(qat(l, i), &code, &act) || act != HP_KEY_ACTION_RELEASE) return false;
    for (size_t j = i + 1; j < l->qn; j++)
        if (key_info(qat(l, j), &c2, &a2) && c2 == code && a2 == HP_KEY_ACTION_RELEASE) return false;
    return true;
}

/* Non-strict, link down: drops the objects older than HP_LINK_STALE_MS. */
static void expire_stale(hp_rf_link_t *l, uint32_t now) {
    for (size_t i = 0; i < l->qn;) {
        if ((int32_t)(now - qat(l, i)->queued_at) < HP_LINK_STALE_MS || keep_release(l, i, now)) {
            i++;
            continue;
        }
        lg(l, now, "dropped type %d: stale while the link is down", (int)qat(l, i)->type);
        l->expired++;
        remove_at(l, i, true);
    }
}

int hp_rf_link_send(hp_rf_link_t *l, hp_msg_type_t type, const uint8_t *body, size_t len, uint32_t now_ms) {
    if (!l || !l->tp || (len && !body) || (unsigned)type > 0xFF) return -HP_E_INVAL;
    if (len > HP_RF_BODY_MAX) return -HP_E_TOOBIG;
    l->now_dbg = now_ms;
    if (!l->speak_first && l->tp->attached && !l->tp->attached(l->tp->self)) return -HP_E_NOENT;
    hp_rf_link_obj_t nw;
    nw.type = type;
    nw.len = len;
    nw.queued_at = now_ms;
    if (len) memcpy(nw.body, body, len);
    uint32_t kcode = 0, kact = 0;
    bool is_key = key_info(&nw, &kcode, &kact);
    size_t first = l->started ? 1 : 0; /* the object in flight stays */
    if (is_key && kact == HP_KEY_ACTION_HOLD) { /* at most one unstarted HOLD per key: a repeat adds nothing */
        for (size_t i = first; i < l->qn; i++) {
            uint32_t c, a;
            if (key_info(qat(l, i), &c, &a) && c == kcode && a == HP_KEY_ACTION_HOLD) return 0;
        }
    } else if (is_key && kact == HP_KEY_ACTION_RELEASE) { /* pending HOLDs of the key are moot */
        for (size_t i = first; i < l->qn;) {
            uint32_t c, a;
            if (key_info(qat(l, i), &c, &a) && c == kcode && a == HP_KEY_ACTION_HOLD) remove_at(l, i, false);
            else i++;
        }
        if (l->qn == HP_LINK_QUEUE) { /* never refused: an unstarted HOLD of any key goes, else the newest unstarted non-key object */
            size_t victim = HP_LINK_QUEUE;
            for (size_t i = first; i < l->qn && victim == HP_LINK_QUEUE; i++) {
                uint32_t c, a;
                if (key_info(qat(l, i), &c, &a) && a == HP_KEY_ACTION_HOLD) victim = i;
            }
            for (size_t i = l->qn; i > first && victim == HP_LINK_QUEUE; i--)
                if (qat(l, i - 1)->type != HP_MSG_KEY_EVENT) victim = i - 1;
            if (victim != HP_LINK_QUEUE) remove_at(l, victim, true);
        }
    }
    if (l->qn == HP_LINK_QUEUE) return -HP_E_BUSY;
    if (!l->tx.send) hp_rf_tx_init(&l->tx, emit, l);
    /* A key goes ahead of the objects that have not started transmitting (remote_info, battery after a resume),
     * behind the one in flight and the keys already waiting. */
    size_t pos = l->qn;
    if (is_key) {
        pos = first; /* never ahead of an unstarted remote_info: it must reach the hub (and reset its filter) before any key */
        for (size_t i = first; i < l->qn; i++)
            if (qat(l, i)->type == HP_MSG_REMOTE_INFO) pos = i + 1;
        while (pos < l->qn && qat(l, pos)->type == HP_MSG_KEY_EVENT) pos++;
    }
    for (size_t k = l->qn; k > pos; k--) *qat(l, k) = *qat(l, k - 1);
    *qat(l, pos) = nw;
    l->qn++;
    if (pos == 0) { /* nothing ahead of it: transmit now, so "no peer" is reported synchronously */
        l->start_wait = false;
        l->start_tries = 0;
        l->noent = false;
        l->sending = true; /* the caller learns of a refusal from the return value: no result callback for it */
        drive_tx(l, now_ms);
        l->sending = false;
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
    l->last_rx = l->now_dbg;
    /* An uplink frame that ARRIVED after the write finished proves the downlink frame was delivered: the radio
     * sends no per-frame TX status while the link is up. Arrival and write are the transport's own stamps
     * (the reader thread's clock, not when we read); the margin covers a frame already in flight at the write. */
    uint32_t rx_at, tx_at;
    if (strict_on(l) && l->out_busy && l->out_wr_ok && l->tp->frame_times &&
        l->tp->frame_times(l->tp->self, &rx_at, &tx_at) && (int32_t)(rx_at - l->out_wr) >= HP_LINK_UPLINK_RELEASE_MS)
        out_done(l, true, REL_UPLINK);
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

static void report(hp_rf_link_t *l, const hp_rf_link_obj_t *o, bool ok) {
    if (l->result) l->result(l->result_ctx, o->type, o->body, o->len, ok);
}

static void drive_tx(hp_rf_link_t *l, uint32_t now) {
    if (l->qn && !strict_on(l) && !link_up(l)) expire_stale(l, now);
    for (int guard = 0; guard < HP_LINK_QUEUE + 1 && l->qn; guard++) {
        hp_rf_link_obj_t *o = &l->q[l->qhead];
        if (!l->started) {
            if (l->start_wait && (int32_t)(now - l->start_at) < 0) return;
            l->start_wait = false;
            l->tx.pace_ms = l->gap_ms;
            lg(l, now, "send start type %d len %zu seq %u", (int)o->type, o->len, (unsigned)(l->tx.next_seq & 0x0F));
            int rc = hp_rf_tx_start(&l->tx, o->type, o->body, o->len, now);
            if (rc == -HP_E_IO && l->last_rc == -HP_E_NOENT) { /* nobody there: not a retryable failure */
                l->start_tries = 0;
                l->noent = true;
                if (!l->sending) report(l, o, false); /* a queued object: its owner counts it in flight */
                pop(l);
                continue;
            }
            if (rc == -HP_E_IO && (l->last_rc == -HP_E_BUSY || (!strict_on(l) && !link_up(l)))) {
                /* transport backoff, or the link is down: not a try; again when the backoff ends (down: until stale) */
                l->start_at = now + (l->last_rc == -HP_E_BUSY ? HP_LINK_WRITE_RETRY_MS : HP_RF_RETRY_MS);
                l->start_wait = true;
                return;
            }
            if (rc == -HP_E_IO) { /* transport refused the first frame (chip error): retry later */
                if (++l->start_tries > HP_RF_RETRIES) {
                    l->failures++;
                    report(l, o, false);
                    l->start_tries = 0;
                    pop(l);
                    continue;
                }
                l->start_at = now + HP_RF_RETRY_MS;
                l->start_wait = true;
                return;
            }
            l->start_tries = 0;
            if (rc < 0) { /* unsendable object: drop it */
                l->failures++;
                report(l, o, false);
                pop(l);
                continue;
            }
            l->started = true;
            return;
        }
        bool hold = strict_on(l) ? (l->out_busy || l->pd_v || !link_up(l)) : !link_up(l);
        if (strict_on(l)) {
            if (!hold) (void)hp_rf_tx_service(&l->tx, now, 1); /* radio free: the next frame of the object */
        } else if (!hold) {
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
        case HP_RF_TX_OK: lg(l, now, "tx done: object acked (seq %u)", (unsigned)l->tx.seq); report(l, o, true); pop(l); continue;
        case HP_RF_TX_FAILED: lg(l, now, "tx FAILED after retries (seq %u)", (unsigned)l->tx.seq); l->failures++; report(l, o, false); pop(l); continue;
        default: return;
        }
    }
}

static uint32_t poll_iv(const hp_rf_link_t *l, uint32_t now) {
    bool waiting = l->qn && l->started && l->tx.state == HP_RF_TX_WAIT && (strict_on(l) || link_up(l));
    bool partial = l->as.active && (int32_t)(now - l->last_rx) < 1000; /* the peer has more frames of an object */
    return waiting || partial || !l->idle_poll_ms ? l->poll_ms : l->idle_poll_ms;
}

/* ms until the next poll is due (0 = now), -1 when not polling. */
static int poll_due(const hp_rf_link_t *l, uint32_t now) {
    if (!l->poll_ms || !l->awake) return -1;
    int32_t d = (int32_t)(l->last_tx + poll_iv(l, now) - now);
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
            if (strict_on(l)) out_done(l, st == 0x00, st == 0x00 ? REL_ST00 : REL_ST01); /* a status with nothing in the radio (e.g. at link-up) is ignored */
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
    if (strict_on(l) && !l->out_busy && (l->pa_v || l->pd_v) && link_up(l)) { /* a refused write: wake when the backoff ends */
        int32_t d = (int32_t)(l->wr_retry_at - now_ms);
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    if (!l->qn) return best;
    if (!strict_on(l) && !link_up(l)) { /* down: no retry clock; wake only for the next stale drop (the attach event wakes the loop itself) */
        int32_t e = INT32_MAX;
        for (size_t i = 0; i < l->qn; i++) {
            const hp_rf_link_obj_t *o = &l->q[(l->qhead + i) % HP_LINK_QUEUE];
            int32_t r = (int32_t)(o->queued_at + HP_LINK_RELEASE_KEEP_MS - now_ms);
            if (o->type != HP_MSG_KEY_EVENT) r = (int32_t)(o->queued_at + HP_LINK_STALE_MS - now_ms);
            else r = r < 0 ? 0 : r; /* a key may be kept (newest RELEASE) up to the longer cap; the earlier bound is harmless */
            if (r < e) e = r;
        }
        if (e < 0) e = 0;
        return best < 0 || e < best ? e : best;
    }
    uint32_t at = l->started ? l->tx.deadline : l->start_wait ? l->start_at : now_ms;
    if (l->started && l->tx.state == HP_RF_TX_WAIT && l->tx.sent < l->tx.n) at = l->tx.next_at; /* pacing */
    int32_t d = (int32_t)(at - now_ms);
    if (d < 0) d = 0;
    return best < 0 || d < best ? d : best;
}

hp_rf_link_status_t hp_rf_link_status(const hp_rf_link_t *l) {
    return (hp_rf_link_status_t){l->qn, l->failures, l->delivered, l->expired};
}
