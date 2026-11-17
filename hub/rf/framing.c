#include "hub/rf/framing.h"

#include <string.h>

#include "hub/hp_errors.h"

#define SEQ_MASK 0x0f
#define KIND_MASK 0xe0

int hp_rf_split(hp_msg_type_t t, uint8_t seq, const uint8_t *body, size_t len,
                hp_rf_inner_t *out, size_t out_cap, size_t *out_n)
{
    if (!out || !out_n || (len && !body) || (unsigned)t > 0xff) return -HP_E_INVAL;
    if (len > HP_RF_BODY_MAX) return -HP_E_TOOBIG;
    size_t total = len + 1; /* length byte + body */
    size_t n = (total + HP_RF_CHUNK - 1) / HP_RF_CHUNK;
    if (n > out_cap) return -HP_E_TOOBIG;

    for (size_t i = 0; i < n; i++) {
        uint8_t *b = out[i].b;
        size_t off = i * HP_RF_CHUNK;
        size_t chunk = total - off < HP_RF_CHUNK ? total - off : HP_RF_CHUNK;
        memset(b, 0, HP_RF_INNER_LEN);
        b[0] = (uint8_t)((i ? HP_RF_KIND_CONT : (HP_RF_KIND_OBJECT | HP_RF_FLAG_FIRST)) | (seq & SEQ_MASK));
        b[1] = (uint8_t)t;
        b[2] = (uint8_t)i;
        for (size_t k = 0; k < chunk; k++) {
            size_t pos = off + k; /* position in the object: 0 = length byte */
            b[3 + k] = pos == 0 ? (uint8_t)len : body[pos - 1];
        }
    }
    *out_n = n;
    return 0;
}

void hp_rf_make_ack(uint8_t type, uint8_t seq, hp_rf_inner_t *out)
{
    memset(out->b, 0, HP_RF_INNER_LEN);
    out->b[0] = (uint8_t)(HP_RF_KIND_ACK | (seq & SEQ_MASK));
    out->b[1] = type;
}

void hp_rf_wrap_hub(uint8_t slot, const hp_rf_inner_t *in, hp_rf_frame_t *out)
{
    out->data[0] = 0x12;
    out->data[1] = slot;
    memcpy(out->data + 2, in->b, HP_RF_INNER_LEN);
    out->len = HP_RF_HUB_FRAME_LEN;
}

int hp_rf_unwrap_hub(const uint8_t *f, size_t len, uint8_t *slot, hp_rf_inner_t *in)
{
    if (!f || !slot || !in || len != HP_RF_HUB_FRAME_LEN || f[0] != 0x12) return -HP_E_INVAL;
    *slot = f[1];
    memcpy(in->b, f + 2, HP_RF_INNER_LEN);
    return 0;
}

void hp_rf_wrap_remote(uint8_t slot, uint8_t powermode, const hp_rf_inner_t *in, hp_rf_frame_t *out)
{
    out->data[0] = 0x20;
    out->data[1] = (uint8_t)((slot & 0x0f) | powermode);
    out->data[2] = 0x12;
    out->data[3] = slot;
    memcpy(out->data + 4, in->b, HP_RF_INNER_LEN);
    out->len = HP_RF_REMOTE_FRAME_LEN;
}

int hp_rf_unwrap_remote(const uint8_t *f, size_t len, uint8_t *slot, uint8_t *byte1, hp_rf_inner_t *in)
{
    if (!f || !slot || !byte1 || !in || len != HP_RF_REMOTE_FRAME_LEN || f[0] != 0x20 || f[2] != 0x12)
        return -HP_E_INVAL;
    *byte1 = f[1];
    *slot = f[3];
    memcpy(in->b, f + 4, HP_RF_INNER_LEN);
    return 0;
}

void hp_rf_asm_init(hp_rf_asm_t *a)
{
    memset(a, 0, sizeof *a);
}

static int abandon(hp_rf_asm_t *a)
{
    a->active = 0;
    return -HP_E_INVAL;
}

int hp_rf_asm_push(hp_rf_asm_t *a, const hp_rf_inner_t *in, hp_rf_msg_t *out, hp_rf_inner_t *ack)
{
    if (!a || !in || !out || !ack) return -HP_E_INVAL;
    const uint8_t *b = in->b;
    uint8_t kind = b[0] & KIND_MASK, seq = b[0] & SEQ_MASK, type = b[1];
    int first = (b[0] & HP_RF_FLAG_FIRST) != 0;

    if (kind == HP_RF_KIND_ACK) {
        out->type = (hp_msg_type_t)type;
        out->seq = seq;
        out->len = 0;
        return HP_ASM_ACK;
    }
    if (kind == HP_RF_KIND_OBJECT) {
        if (!first || b[3] > HP_RF_BODY_MAX) return abandon(a); /* 255 would overflow len+body */
        a->active = 1;
        a->type = type;
        a->seq = seq;
        a->next_idx = 1;
        a->len = b[3];
        a->have = 0;
    } else if (kind == HP_RF_KIND_CONT) {
        if (first || !a->active || a->type != type || a->seq != seq || b[2] != a->next_idx)
            return abandon(a);
        a->next_idx++;
    } else {
        return abandon(a);
    }

    size_t need = (size_t)a->len + 1, left = need - a->have;
    size_t chunk = left < HP_RF_CHUNK ? left : HP_RF_CHUNK;
    memcpy(a->buf + a->have, b + 3, chunk);
    a->have += chunk;
    if (a->have < need) return HP_ASM_INCOMPLETE;

    a->active = 0;
    hp_rf_make_ack(a->type, a->seq, ack);
    if (a->have_last && a->last_seq == a->seq) return HP_ASM_DUPLICATE;
    a->have_last = 1;
    a->last_seq = a->seq;
    out->type = (hp_msg_type_t)a->type;
    out->seq = a->seq;
    out->len = a->len;
    if (a->len) memcpy(out->payload, a->buf + 1, a->len);
    return HP_ASM_COMPLETE;
}

/* ---- sender ---- */

void hp_rf_tx_init(hp_rf_tx_t *tx, hp_rf_send_fn send, void *ctx)
{
    memset(tx, 0, sizeof *tx);
    tx->send = send;
    tx->ctx = ctx;
}

static int tx_send_all(hp_rf_tx_t *tx)
{
    for (size_t i = 0; i < tx->n; i++)
        if (tx->send(tx->ctx, &tx->fr[i]) < 0) return -HP_E_IO;
    return 0;
}

int hp_rf_tx_start(hp_rf_tx_t *tx, hp_msg_type_t t, const uint8_t *body, size_t len, uint32_t now_ms)
{
    if (!tx || !tx->send) return -HP_E_INVAL;
    if (tx->state == HP_RF_TX_WAIT) return -HP_E_BUSY;
    uint8_t seq = tx->next_seq & SEQ_MASK;
    size_t n = 0;
    int rc = hp_rf_split(t, seq, body, len, tx->fr, HP_RF_MAX_FRAGS, &n);
    if (rc < 0) return rc;
    tx->n = n;
    tx->seq = seq;
    tx->type = (uint8_t)t;
    tx->retries = 0;
    if (tx_send_all(tx) < 0) {
        tx->state = HP_RF_TX_IDLE;
        return -HP_E_IO;
    }
    tx->next_seq = (uint8_t)((seq + 1) & SEQ_MASK);
    tx->deadline = now_ms + HP_RF_RETRY_MS;
    tx->state = HP_RF_TX_WAIT;
    return 0;
}

int hp_rf_tx_on_ack(hp_rf_tx_t *tx, uint8_t type, uint8_t seq)
{
    if (!tx || tx->state != HP_RF_TX_WAIT || tx->type != type || tx->seq != (seq & SEQ_MASK)) return 0;
    tx->state = HP_RF_TX_OK;
    return 1;
}

hp_rf_tx_state_t hp_rf_tx_poll(hp_rf_tx_t *tx, uint32_t now_ms)
{
    if (tx->state != HP_RF_TX_WAIT || (int32_t)(now_ms - tx->deadline) < 0) return tx->state;
    if (tx->retries >= HP_RF_RETRIES) {
        tx->state = HP_RF_TX_FAILED;
        return tx->state;
    }
    tx->retries++;
    (void)tx_send_all(tx); /* a failed attempt still counts; the next timeout retries */
    tx->deadline = now_ms + HP_RF_RETRY_MS;
    return tx->state;
}
