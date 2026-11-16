#include "hub/rf/rf_payload.h"

#include <string.h>

#include "hub/hp_errors.h"
#include "hub/rf/framing.h"

static int wire_size(const hp_field_desc_t *f) {
    switch (f->type) {
    case HP_T_U8: case HP_T_BOOL: case HP_T_ENUM: return 1;
    case HP_T_U16: return 2;
    case HP_T_U32: case HP_T_I32: return 4;
    case HP_T_STR: case HP_T_BYTES: return 0; /* length-prefixed */
    default: return -1;
    }
}

static const hp_msg_desc_t *rf_desc(hp_msg_type_t t) {
    const hp_msg_desc_t *d = hp_msg_desc(t);
    if (!d || !(d->transport & HP_TRANSPORT_RF)) return NULL;
    for (size_t i = 0; i < d->field_count; i++)
        if (d->fields[i].optional || wire_size(&d->fields[i]) < 0) return NULL;
    return d;
}

static bool enum_ok(const hp_enum_desc_t *e, uint32_t v) {
    for (size_t i = 0; i < e->count; i++)
        if ((uint32_t)e->members[i].value == v) return true;
    return false;
}

int hp_rf_pack(hp_msg_type_t t, const hp_rf_val_t *vals, size_t nvals, uint8_t *out, size_t cap,
               size_t *out_len) {
    const hp_msg_desc_t *d = rf_desc(t);
    if (!d || !vals || !out || !out_len || nvals != d->field_count) return -HP_E_INVAL;
    size_t o = 0;
    for (size_t i = 0; i < d->field_count; i++) {
        const hp_field_desc_t *f = &d->fields[i];
        const hp_rf_val_t *v = &vals[i];
        int w = wire_size(f);
        if (w == 0) {
            if (v->len > 255) return -HP_E_TOOBIG;
            if (v->len && !v->p) return -HP_E_INVAL;
            if (o + 1 + v->len > HP_RF_MSG_MAX || o + 1 + v->len > cap) return -HP_E_TOOBIG;
            out[o++] = (uint8_t)v->len;
            if (v->len) memcpy(out + o, v->p, v->len);
            o += v->len;
            continue;
        }
        if (o + (size_t)w > HP_RF_MSG_MAX || o + (size_t)w > cap) return -HP_E_TOOBIG;
        if (f->type == HP_T_U8 && v->u > 0xFF) return -HP_E_INVAL;
        if (f->type == HP_T_U16 && v->u > 0xFFFF) return -HP_E_INVAL;
        if (f->type == HP_T_BOOL && v->u > 1) return -HP_E_INVAL;
        if (f->type == HP_T_ENUM && (v->u > 0xFF || !enum_ok(f->enum_desc, v->u))) return -HP_E_INVAL;
        for (int b = w - 1; b >= 0; b--) out[o++] = (uint8_t)(v->u >> (8 * b));
    }
    *out_len = o;
    return 0;
}

int hp_rf_unpack(hp_msg_type_t t, const uint8_t *in, size_t len, hp_rf_val_t *vals, size_t nvals) {
    const hp_msg_desc_t *d = rf_desc(t);
    if (!d || (len && !in) || !vals || nvals < d->field_count) return -HP_E_INVAL;
    size_t o = 0;
    for (size_t i = 0; i < d->field_count; i++) {
        const hp_field_desc_t *f = &d->fields[i];
        hp_rf_val_t *v = &vals[i];
        int w = wire_size(f);
        memset(v, 0, sizeof *v);
        if (w == 0) {
            if (o + 1 > len) return -HP_E_INVAL;
            size_t n = in[o++];
            if (o + n > len) return -HP_E_INVAL;
            v->p = in + o;
            v->len = n;
            o += n;
            continue;
        }
        if (o + (size_t)w > len) return -HP_E_INVAL;
        uint32_t u = 0;
        for (int b = 0; b < w; b++) u = (u << 8) | in[o++];
        if (f->type == HP_T_BOOL && u > 1) return -HP_E_INVAL;
        if (f->type == HP_T_ENUM && !enum_ok(f->enum_desc, u)) return -HP_E_INVAL;
        v->u = u;
    }
    return o == len ? 0 : -HP_E_INVAL;
}
