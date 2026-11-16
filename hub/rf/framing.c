#include "hub/rf/framing.h"

#include <string.h>

#include "hub/hp_errors.h"

int hp_rf_split(hp_msg_type_t t, uint8_t seq, const uint8_t *payload, size_t len,
                size_t frame_cap, hp_rf_frame_t *out, size_t out_cap, size_t *out_n)
{
    if (!out || !out_n || (len && !payload)) return -HP_E_INVAL;
    if (frame_cap <= HP_RF_HDR_LEN || frame_cap > HP_RF_FRAME_MAX) return -HP_E_INVAL;
    if ((unsigned)t > 0xff) return -HP_E_INVAL;

    size_t per = frame_cap - HP_RF_HDR_LEN;
    size_t n = len ? (len + per - 1) / per : 1;
    if (n > HP_RF_MAX_FRAGS || n > out_cap) return -HP_E_TOOBIG;

    for (size_t i = 0; i < n; i++) {
        size_t off = i * per;
        size_t chunk = len - off < per ? len - off : per;
        if (len == 0) chunk = 0;
        hp_rf_frame_t *f = &out[i];
        f->data[0] = (uint8_t)t;
        f->data[1] = seq;
        f->data[2] = (uint8_t)((i << 4) | n);
        f->data[3] = (uint8_t)chunk;
        if (chunk) memcpy(f->data + HP_RF_HDR_LEN, payload + off, chunk);
        f->len = HP_RF_HDR_LEN + chunk;
    }
    *out_n = n;
    return 0;
}

void hp_rf_asm_init(hp_rf_asm_t *a)
{
    memset(a, 0, sizeof *a);
}

int hp_rf_asm_push(hp_rf_asm_t *a, const uint8_t *frame, size_t len, hp_rf_msg_t *out)
{
    if (!a || !frame || !out || len < HP_RF_HDR_LEN) return -HP_E_INVAL;

    uint8_t type = frame[0], seq = frame[1];
    unsigned index = frame[2] >> 4, total = frame[2] & 0x0f;
    size_t flen = frame[3];

    if (total == 0 || index >= total) return -HP_E_INVAL;
    if (flen > HP_RF_FRAG_PAYLOAD_MAX || flen > len - HP_RF_HDR_LEN) return -HP_E_INVAL;

    int same = a->active && a->type == type && a->seq == seq;
    if (same && a->total != total) return -HP_E_INVAL;

    if (!same) { /* new message: discard any partial */
        hp_rf_asm_init(a);
        a->active = 1;
        a->type = type;
        a->seq = seq;
        a->total = (uint8_t)total;
    }

    if (!(a->mask & (1u << index))) { /* duplicates are ignored */
        if (flen) memcpy(a->buf + index * HP_RF_FRAG_PAYLOAD_MAX, frame + HP_RF_HDR_LEN, flen);
        a->lens[index] = (uint8_t)flen;
        a->mask |= (uint16_t)(1u << index);
    }

    if (a->mask != (uint16_t)((1u << total) - 1)) return HP_ASM_INCOMPLETE;

    size_t off = 0;
    for (unsigned i = 0; i < total; i++) {
        memcpy(out->payload + off, a->buf + i * HP_RF_FRAG_PAYLOAD_MAX, a->lens[i]);
        off += a->lens[i];
    }
    out->type = (hp_msg_type_t)a->type;
    out->seq = a->seq;
    out->len = off;
    hp_rf_asm_init(a);
    return HP_ASM_COMPLETE;
}
