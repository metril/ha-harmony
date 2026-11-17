#include "hub/rf/link.h"

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
    hp_rf_asm_init(&l->as);
}

static int emit(void *vl, const hp_rf_inner_t *fr) {
    hp_rf_link_t *l = vl;
    int rc;
    if (l->wrapped) {
        hp_rf_frame_t w;
        hp_rf_wrap_hub(l->slot, fr, &w);
        rc = l->tp->send(l->tp->self, w.data, w.len);
    } else {
        rc = l->tp->send(l->tp->self, fr->b, HP_RF_INNER_LEN);
    }
    l->last_rc = rc;
    return rc;
}

static void reset(hp_rf_link_t *l) {
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
}

static void drive_tx(hp_rf_link_t *l, uint32_t now);

int hp_rf_link_send(hp_rf_link_t *l, hp_msg_type_t type, const uint8_t *body, size_t len, uint32_t now_ms) {
    if (!l || !l->tp || (len && !body) || (unsigned)type > 0xFF) return -HP_E_INVAL;
    if (len > HP_RF_BODY_MAX) return -HP_E_TOOBIG;
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

static void send_ack(hp_rf_link_t *l, const hp_rf_inner_t *ack) { (void)emit(l, ack); }

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
    switch (hp_rf_asm_push(&l->as, &in, &msg, &ack)) {
    case HP_ASM_COMPLETE:
        l->delivered++;
        send_ack(l, &ack);
        if (l->deliver) l->deliver(l->deliver_ctx, &msg);
        break;
    case HP_ASM_DUPLICATE: send_ack(l, &ack); break;
    case HP_ASM_ACK: (void)hp_rf_tx_on_ack(&l->tx, (uint8_t)msg.type, msg.seq); break;
    default: break;
    }
}

static void drive_tx(hp_rf_link_t *l, uint32_t now) {
    for (int guard = 0; guard < HP_LINK_QUEUE + 1 && l->qn; guard++) {
        hp_rf_link_obj_t *o = &l->q[l->qhead];
        if (!l->started) {
            if (l->start_tries && (int32_t)(now - l->start_at) < 0) return;
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
        switch (hp_rf_tx_poll(&l->tx, now)) {
        case HP_RF_TX_OK: pop(l); continue;
        case HP_RF_TX_FAILED: l->failures++; pop(l); continue;
        default: return;
        }
    }
}

int hp_rf_link_pump(hp_rf_link_t *l, uint32_t now_ms) {
    uint8_t fr[HP_RF_FRAME_MAX + 1];
    int rc = 0, n;
    if (!l->tx.send) hp_rf_tx_init(&l->tx, emit, l);
    while ((n = l->tp->recv(l->tp->self, fr, sizeof fr, 0)) != 0) {
        if (n < 0) {
            if (n == -HP_E_NOENT) reset(l);
            rc = n;
            break;
        }
        on_frame(l, fr, (size_t)n);
    }
    drive_tx(l, now_ms);
    if (l->noent) { /* a queued object found nobody home */
        l->noent = false;
        l->failures++;
    }
    return rc;
}

int hp_rf_link_next_timeout_ms(const hp_rf_link_t *l, uint32_t now_ms) {
    if (!l->qn) return -1;
    uint32_t at = l->started ? l->tx.deadline : l->start_tries ? l->start_at : now_ms;
    int32_t d = (int32_t)(at - now_ms);
    return d < 0 ? 0 : d;
}

hp_rf_link_status_t hp_rf_link_status(const hp_rf_link_t *l) {
    return (hp_rf_link_status_t){l->qn, l->failures, l->delivered};
}
