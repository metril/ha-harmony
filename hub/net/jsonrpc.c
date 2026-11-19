#include "hub/net/jsonrpc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hub/hp_errors.h"
#include "hub/net/ws.h"

int hp_make_reply(uint32_t id, const char *type, cJSON *d, char *reply, size_t cap, size_t *len) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(d);
        return -HP_E_INVAL;
    }
    cJSON_AddNumberToObject(root, "v", HP_PROTO_VERSION);
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "t", type);
    if (!d) d = cJSON_CreateObject();
    if (!d) {
        cJSON_Delete(root);
        return -HP_E_INVAL;
    }
    cJSON_AddItemToObject(root, "d", d);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -HP_E_INVAL;
    size_t n = strlen(s);
    int rc = 0;
    if (n + 1 > cap) {
        rc = -HP_E_TOOBIG;
    } else {
        memcpy(reply, s, n + 1);
        *len = n;
    }
    free(s);
    return rc;
}

int hp_make_error(uint32_t id, const char *code, const char *message, char *reply, size_t cap,
                  size_t *len) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return -HP_E_INVAL;
    cJSON_AddStringToObject(d, "code", code);
    cJSON_AddStringToObject(d, "message", message);
    return hp_make_reply(id, "error", d, reply, cap, len);
}

static bool is_int_in(const cJSON *v, double lo, double hi) {
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble == floor(v->valuedouble) &&
           v->valuedouble >= lo && v->valuedouble <= hi;
}

static bool field_ok(const hp_field_desc_t *f, const cJSON *v) {
    switch (f->type) {
    case HP_T_U8: return is_int_in(v, 0, 255);
    case HP_T_U16: return is_int_in(v, 0, 65535);
    case HP_T_U32: return is_int_in(v, 0, 4294967295.0);
    case HP_T_I32: return is_int_in(v, -2147483648.0, 2147483647.0);
    case HP_T_BOOL: return cJSON_IsBool(v);
    case HP_T_STR:
        return cJSON_IsString(v) && hp_utf8_valid((const uint8_t *)v->valuestring, strlen(v->valuestring));
    case HP_T_BYTES:
        return cJSON_IsString(v) && hp_base64_valid(v->valuestring, strlen(v->valuestring));
    case HP_T_JSON: return cJSON_IsObject(v) || cJSON_IsArray(v);
    case HP_T_ENUM: {
        if (!is_int_in(v, -2147483648.0, 2147483647.0)) return false;
        const hp_enum_desc_t *e = f->enum_desc;
        if (!e) return false;
        for (size_t i = 0; i < e->count; i++)
            if (e->members[i].value == (int32_t)v->valuedouble) return true;
        return false;
    }
    }
    return false;
}

/* Returns NULL when valid, else a static description. */
static const char *validate_d(const hp_msg_desc_t *md, const cJSON *d) {
    if (d && !cJSON_IsObject(d)) return "d must be an object";
    /* Fields we do not know are ignored: a newer peer may add optional fields. */
    for (size_t i = 0; i < md->field_count; i++) {
        const cJSON *v = d ? cJSON_GetObjectItemCaseSensitive((cJSON *)d, md->fields[i].name) : NULL;
        if (!v) {
            if (!md->fields[i].optional) return "missing field";
            continue;
        }
        if (!field_ok(&md->fields[i], v)) return "bad field type or range";
    }
    return NULL;
}

static bool token_ok(const char *want, const char *got) {
    if (!want || !*want || !got) return false;
    size_t wl = strlen(want), gl = strlen(got);
    unsigned diff = (unsigned)(wl ^ gl);
    for (size_t i = 0; i < wl; i++) {
        unsigned char g = i < gl ? (unsigned char)got[i] : 0;
        diff |= (unsigned)((unsigned char)want[i] ^ g);
    }
    return diff == 0;
}

#define ERR(id, code, msg) hp_make_error((id), (code), (msg), reply, reply_cap, reply_len)

int hp_dispatch(const char *json, size_t len, const hp_handler_tbl_t *tbl, void *user, char *reply,
                size_t reply_cap, size_t *reply_len) {
    if (!json || !tbl || !user || !reply || !reply_len) return -HP_E_INVAL;
    hp_session_t *sess = user;
    *reply_len = 0;
    int rc;

    if (!hp_utf8_valid((const uint8_t *)json, len)) return ERR(0, "bad_request", "invalid utf-8");
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ERR(0, "bad_request", "invalid json");
    if (!cJSON_IsObject(root)) {
        rc = ERR(0, "bad_request", "envelope must be an object");
        goto out;
    }
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!is_int_in(jid, 0, 4294967295.0)) {
        rc = ERR(0, "bad_request", "id must be u32");
        goto out;
    }
    uint32_t id = (uint32_t)jid->valuedouble;
    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!is_int_in(jv, 0, 4294967295.0)) {
        rc = ERR(id, "bad_request", "v must be an integer");
        goto out;
    }
    if (jv->valuedouble != (double)HP_PROTO_VERSION) {
        rc = ERR(id, "version", "unsupported protocol version");
        goto out;
    }
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(jt)) {
        rc = ERR(id, "bad_request", "t must be a string");
        goto out;
    }
    const hp_msg_desc_t *md = hp_msg_desc_by_name(jt->valuestring);
    if (!md || md->transport == HP_TRANSPORT_RF) {
        rc = ERR(id, "unknown_type", "unknown message type");
        goto out;
    }
    if (!sess->authed && md->type != HP_MSG_HELLO) {
        rc = ERR(id, "unauthorized", "hello required");
        if (rc == 0) rc = HP_DISPATCH_CLOSE;
        goto out;
    }
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "d");
    const char *bad = validate_d(md, d);
    if (bad) {
        rc = ERR(id, "bad_request", bad);
        goto out;
    }

    if (md->type == HP_MSG_HELLO) {
        const cJSON *tok = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "token");
        if (!token_ok(tbl->token, tok->valuestring)) {
            sess->authed = false;
            rc = ERR(id, "unauthorized", "bad token");
            if (rc == 0) rc = HP_DISPATCH_CLOSE;
            goto out;
        }
        sess->authed = true;
        sess->bt_listen = false;
        cJSON *ack = cJSON_CreateObject();
        if (!ack) {
            rc = -HP_E_INVAL;
            goto out;
        }
        cJSON_AddStringToObject(ack, "hub_id", tbl->hub_id ? tbl->hub_id : "");
        if (tbl->fw_version) cJSON_AddStringToObject(ack, "fw_version", tbl->fw_version);
        if (tbl->activities) {
            cJSON *acts = tbl->activities(tbl->ctx);
            if (acts) cJSON_AddItemToObject(ack, "activities", acts);
        }
        char hash[9];
        if (tbl->config_hash && tbl->config_hash(tbl->ctx, hash)) cJSON_AddStringToObject(ack, "config_hash", hash);
        if (tbl->limits) {
            cJSON *lim = tbl->limits(tbl->ctx);
            if (lim) cJSON_AddItemToObject(ack, "limits", lim);
        }
        if (tbl->hello_extra) tbl->hello_extra(tbl->ctx, ack);
        rc = hp_make_reply(id, "hello_ack", ack, reply, reply_cap, reply_len);
        goto out;
    }
    hp_handler_fn h = tbl->handler[md->type];
    if (!h) {
        if (md->type == HP_MSG_PING) {
            rc = hp_make_reply(id, "pong", NULL, reply, reply_cap, reply_len);
        } else {
            rc = ERR(id, "unknown_type", "no handler for message type");
        }
        goto out;
    }
    rc = h(tbl->ctx, sess, id, d, reply, reply_cap, reply_len);
out:
    cJSON_Delete(root);
    return rc;
}
