#define _POSIX_C_SOURCE 200809L
#include "hub/hub_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hub/hp_errors.h"
#include "hub/rf/rf_payload.h"

static uint32_t mono_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void hub_notify(void *vctx, hp_actsm_evt_t evt, int idx) {
    hub_ctx_t *h = vctx;
    if (evt == HP_ACTSM_EVT_DEVICE) {
        h->pending_dev |= 1u << idx;
    } else {
        h->pending_state = true;
        h->pending_activity = idx < 0 ? 0 : idx;
        h->pending_power = idx >= 0;
    }
}

void hub_ctx_init(hub_ctx_t *c) {
    memset(c, 0, sizeof *c);
    hp_map_init(&c->map);
    hp_rf_asm_init(&c->rf_asm);
    hp_sched_init(&c->sched, mono_ms, NULL);
    hp_irout_init(&c->irout, &c->sched);
    hp_map_set_out(&c->map, &c->irout);
    hp_actsm_init(&c->act, &c->map, &c->sched, &c->sinks, hub_notify, c);
}

void hub_set_clock(hub_ctx_t *c, hp_clock_fn fn, void *ctx) {
    c->sched.clock = fn;
    c->sched.clock_ctx = ctx;
}

int hub_next_timeout_ms(const hub_ctx_t *c) { return hp_sched_next_timeout_ms(&c->sched); }
void hub_run_timers(hub_ctx_t *c) {
    hp_sched_run(&c->sched, hp_sched_now(&c->sched));
    hub_flush_pending(c); /* a transition may have completed / changed device state */
}

void hub_shutdown(hub_ctx_t *c) {
    if (hp_actsm_flush_state(&c->act) < 0) fprintf(stderr, "hub: final state write failed\n");
}

#define HUB_RF_CAP 30 /* hub->remote total frame size */

/* Packs, splits and sends one RF message. 0, -HP_E_TOOBIG (nothing sent),
 * -HP_E_NOENT (no remote; nothing sent), other negative: link failed. */
static int rf_send_msg(hub_ctx_t *h, hp_msg_type_t t, const hp_rf_val_t *v, size_t nv) {
    uint8_t payload[HP_RF_MSG_MAX];
    hp_rf_frame_t fr[HP_RF_MAX_FRAGS];
    size_t plen = 0, nf = 0;
    if (!h->sinks.rf) return -HP_E_NOENT;
    int rc = hp_rf_pack(t, v, nv, payload, sizeof payload, &plen);
    if (rc < 0) return rc;
    rc = hp_rf_split(t, h->rf_seq++, payload, plen, HUB_RF_CAP, fr, HP_RF_MAX_FRAGS, &nf);
    if (rc < 0) return rc;
    for (size_t i = 0; i < nf; i++) {
        rc = h->sinks.rf->send(h->sinks.rf->self, fr[i].data, fr[i].len);
        if (rc < 0) return (i == 0 || rc == -HP_E_NOENT) ? rc : -HP_E_IO;
    }
    return 0;
}

/* ------------------------------------------------------------ broadcast */

typedef struct {
    const char *msg;
    size_t len;
    const hp_session_t *skip;
} bcast_t;

static void bcast_one(hp_ws_conn_t *c, void *arg) {
    const hp_session_t *s = hp_ws_conn_user(c);
    const bcast_t *b = arg;
    if (s && s->authed && s != b->skip) hp_ws_send(c, b->msg, b->len);
}

static void broadcast_except(hub_ctx_t *h, const char *type, cJSON *d, const hp_session_t *skip) {
    char buf[1536]; /* activity_list: 16 names of up to 31 bytes + hash */
    size_t n = 0;
    if (h->srv && hp_make_reply(0, type, d, buf, sizeof buf, &n) == 0) {
        bcast_t b = {buf, n, skip};
        hp_ws_foreach(h->srv, bcast_one, &b);
    } else if (!h->srv) {
        cJSON_Delete(d); /* hp_make_reply not reached: we still own d */
    }
}

void hub_broadcast(hub_ctx_t *h, const char *type, cJSON *d) { broadcast_except(h, type, d, NULL); }

static cJSON *activities_json(const hub_ctx_t *h) {
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    for (size_t i = 0; i < hp_map_activity_count(&h->map); i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(hp_map_activity_name(&h->map, i)));
    return a;
}

static cJSON *device_json(const hub_ctx_t *h, size_t i) {
    const hp_devstate_t *d = hp_actsm_device(&h->act, i);
    cJSON *o = d ? cJSON_CreateObject() : NULL;
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device", d->id);
    cJSON_AddBoolToObject(o, "power", d->power != 0);
    if (d->input[0]) cJSON_AddStringToObject(o, "input", d->input);
    return o;
}

void hub_send_state(hub_ctx_t *h, hp_ws_conn_t *c) {
    int cur = hp_actsm_reported(&h->act);
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "activity", cur < 0 ? 0 : cur);
    cJSON_AddBoolToObject(d, "power", cur >= 0);
    char buf[256];
    size_t n = 0;
    if (hp_make_reply(0, "state_update", d, buf, sizeof buf, &n) == 0) (void)hp_ws_send(c, buf, n);
    for (size_t i = 0; hp_actsm_device(&h->act, i); i++) {
        cJSON *o = device_json(h, i);
        if (o && hp_make_reply(0, "device_state", o, buf, sizeof buf, &n) == 0) (void)hp_ws_send(c, buf, n);
    }
}

void hub_flush_pending(hub_ctx_t *h) {
    if (h->pending_list) {
        h->pending_list = false;
        cJSON *d = cJSON_CreateObject();
        cJSON *acts = activities_json(h);
        if (d && acts) {
            cJSON_AddItemToObject(d, "activities", acts);
            char hash[9];
            if (hp_map_hash_hex(&h->map, hash)) cJSON_AddStringToObject(d, "config_hash", hash);
            broadcast_except(h, "activity_list", d, h->list_skip);
        } else {
            cJSON_Delete(d);
            cJSON_Delete(acts);
        }
        h->list_skip = NULL;
    }
    if (h->pending_dev) {
        uint32_t mask = h->pending_dev;
        h->pending_dev = 0;
        for (size_t i = 0; i < HP_MAP_MAX_DEVICES; i++)
            if ((mask & (1u << i)) && hp_actsm_device(&h->act, i)) hub_broadcast(h, "device_state", device_json(h, i));
    }
    if (!h->pending_state) return;
    h->pending_state = false;
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "activity", h->pending_activity);
    cJSON_AddBoolToObject(d, "power", h->pending_power);
    hub_broadcast(h, "state_update", d);
    hp_rf_val_t v[2] = {{.u = (uint32_t)h->pending_activity & 0xFF}, {.u = h->pending_power ? 1u : 0u}};
    (void)rf_send_msg(h, HP_MSG_STATE_UPDATE, v, 2); /* best effort: remote may be absent */
}

/* ------------------------------------------------------------- handlers */

static int reply_ok(uint32_t id, const char *type, bool ok, const char *msg, char *reply, size_t cap,
                    size_t *len) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return -HP_E_INVAL;
    cJSON_AddBoolToObject(d, "ok", ok);
    if (msg) cJSON_AddStringToObject(d, "message", msg);
    return hp_make_reply(id, type, d, reply, cap, len);
}

static const char *load_err(int rc) {
    switch (-rc) {
    case HP_E_TOOBIG: return "config exceeds a hub limit";
    case HP_E_INVAL: return "invalid config";
    default: return "config could not be loaded";
    }
}

static int on_set_config(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply,
                         size_t cap, size_t *len) {
    hub_ctx_t *h = vctx;
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "config");
    char *text = cJSON_PrintUnformatted(cfg);
    if (!text) return reply_ok(id, "config_ack", false, "out of memory", reply, cap, len);
    int rc = hp_map_load(&h->map, text, strlen(text));
    free(text);
    if (rc < 0) {
        const char *why = hp_map_last_error(&h->map);
        return reply_ok(id, "config_ack", false, *why ? why : load_err(rc), reply, cap, len);
    }
    h->pending_list = true;
    h->list_skip = sess;
    hp_actsm_reloaded(&h->act); /* cancels a transition, re-keys device state, queues device_state/state_update */
    const char *msg = NULL;
    if (h->cache_path && hp_map_save_cache(&h->map, h->cache_path) < 0) msg = "applied; cache write failed";
    return reply_ok(id, "config_ack", true, msg, reply, cap, len);
}

static int reply_err(uint32_t id, const char *code, const char *msg, char *reply, size_t cap, size_t *len);

static int cmd_ir_send(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    if (!cJSON_IsObject(args)) return reply_ok(id, "command_result", false, "args must be an IR action", reply, cap, len);
    if (hp_actsm_busy(&h->act)) return reply_ok(id, "command_result", false, "transition in progress", reply, cap, len);
    hp_map_t *sc = calloc(1, sizeof *sc); /* scratch pool for the action's IR data */
    if (!sc) return reply_ok(id, "command_result", false, "out of memory", reply, cap, len);
    hp_map_set_out(sc, &h->irout);
    hp_action_t a;
    int rc = hp_map_parse_ir(sc, args, &a);
    int r;
    if (rc < 0) {
        r = reply_ok(id, "command_result", false, sc->err[0] ? sc->err : "invalid IR action", reply, cap, len);
    } else if ((rc = hp_map_apply_deferrable(sc, &a, &h->sinks)) < 0) {
        r = reply_ok(id, "command_result", false, h->sinks.ir ? "IR send failed" : "no IR output", reply, cap, len);
    } else {
        r = reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    free(sc);
    return r;
}

static bool u8_arg(const cJSON *o, const char *k, uint32_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255 || v->valuedouble != (double)(uint32_t)v->valuedouble)
        return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}

static int cmd_tile_update(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    uint32_t r, p, t;
    const cJSON *tx = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "text") : NULL;
    if (!cJSON_IsObject(args) || !u8_arg(args, "remote", &r) || !u8_arg(args, "page", &p) ||
        !u8_arg(args, "tile", &t) || !cJSON_IsString(tx))
        return reply_ok(id, "command_result", false, "args need remote, page, tile (0..255) and text", reply, cap, len);
    hp_rf_val_t v[4] = {{.u = r}, {.u = p}, {.u = t}, {.p = (const uint8_t *)tx->valuestring, .len = strlen(tx->valuestring)}};
    int rc = rf_send_msg(h, HP_MSG_TILE_UPDATE, v, 4);
    if (rc == -HP_E_TOOBIG) return reply_err(id, "too_big", "tile text exceeds 255 bytes", reply, cap, len);
    if (rc == -HP_E_NOENT) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    if (rc < 0) return reply_err(id, "rf_error", "RF send failed", reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static int cmd_set_device_state(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    const cJSON *dv = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "device") : NULL;
    const cJSON *pw = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "power") : NULL;
    const cJSON *in = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "input") : NULL;
    if (!cJSON_IsString(dv) || (pw && !cJSON_IsBool(pw)) || (in && !cJSON_IsString(in) && !cJSON_IsNull(in)) || (!pw && !in))
        return reply_ok(id, "command_result", false, "args need device and power and/or input", reply, cap, len);
    const char *input = !in ? NULL : cJSON_IsNull(in) ? "" : in->valuestring;
    int rc = hp_actsm_set_device(&h->act, dv->valuestring, pw ? (cJSON_IsTrue(pw) ? 1 : 0) : -1, input);
    if (rc < 0) return reply_ok(id, "command_result", false, "unknown device or input", reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static int cmd_device_command(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    const cJSON *dv = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "device") : NULL;
    const cJSON *cm = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "command") : NULL;
    if (!cJSON_IsString(dv) || !cJSON_IsString(cm))
        return reply_ok(id, "command_result", false, "args need device and command", reply, cap, len);
    int rc = hp_actsm_device_command(&h->act, dv->valuestring, cm->valuestring);
    if (rc == 0) return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    const char *why = rc == -HP_E_NOENT ? "unknown device"
                      : rc == -HP_E_INVAL ? "unknown command for this device"
                      : rc == -HP_E_BUSY ? "transition in progress"
                      : h->sinks.ir ? "IR send failed" : "no IR output";
    return reply_ok(id, "command_result", false, why, reply, cap, len);
}

static int on_command(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply,
                      size_t cap, size_t *len) {
    (void)sess;
    hub_ctx_t *h = vctx;
    const char *dev = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "device")->valuestring;
    const char *cmd = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "command")->valuestring;
    const cJSON *args = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "args");
    int rc;
    if (!strcmp(dev, "hub") && !strcmp(cmd, "start_activity")) {
        const cJSON *n = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "name") : NULL;
        if (!cJSON_IsString(n)) return reply_ok(id, "command_result", false, "args.name required", reply, cap, len);
        rc = hp_actsm_start(&h->act, n->valuestring);
        if (rc < 0)
            return reply_ok(id, "command_result", false,
                            rc == -HP_E_INVAL ? "unknown activity" : "activity step failed", reply, cap, len);
        return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    if (!strcmp(dev, "hub") && !strcmp(cmd, "power_off")) {
        rc = hp_actsm_off(&h->act);
        if (rc < 0)
            return reply_ok(id, "command_result", false,
                            rc == -HP_E_INVAL ? "no activity running" : "off step failed", reply, cap, len);
        return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    if (!strcmp(dev, "hub") && !strcmp(cmd, "set_device_state")) return cmd_set_device_state(h, id, args, reply, cap, len);
    if (!strcmp(dev, "hub") && !strcmp(cmd, "device_command")) return cmd_device_command(h, id, args, reply, cap, len);
    if (!strcmp(dev, "hub") && !strcmp(cmd, "ir_send")) return cmd_ir_send(h, id, args, reply, cap, len);
    if (!strcmp(dev, "hub") && !strcmp(cmd, "tile_update")) return cmd_tile_update(h, id, args, reply, cap, len);
    return reply_ok(id, "command_result", false, "unknown command", reply, cap, len);
}

static int reply_err(uint32_t id, const char *code, const char *msg, char *reply, size_t cap, size_t *len) {
    return hp_make_error(id, code, msg, reply, cap, len);
}

static int on_page_push(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply,
                        size_t cap, size_t *len) {
    (void)sess;
    hub_ctx_t *h = vctx;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "remote");
    const cJSON *pg = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "page");
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "data");
    uint8_t data[256];
    size_t n = hp_base64_decode(b64->valuestring, strlen(b64->valuestring), data, sizeof data);
    if (n == (size_t)-1 || n > 255) return reply_err(id, "too_big", "page data exceeds 255 bytes", reply, cap, len);
    hp_rf_val_t v[3] = {{.u = (uint32_t)r->valuedouble}, {.u = (uint32_t)pg->valuedouble}, {.p = data, .len = n}};
    int rc = rf_send_msg(h, HP_MSG_PAGE_PUSH, v, 3);
    if (rc == -HP_E_TOOBIG) return reply_err(id, "too_big", "page does not fit the RF budget", reply, cap, len);
    if (rc == -HP_E_NOENT) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    if (rc < 0) return reply_err(id, "rf_error", "RF send failed", reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static cJSON *on_activities(void *vctx) { return activities_json(vctx); }

static bool on_config_hash(void *vctx, char out[9]) {
    hub_ctx_t *h = vctx;
    return hp_map_hash_hex(&h->map, out);
}

void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx) {
    tbl->ctx = ctx;
    tbl->activities = on_activities;
    tbl->config_hash = on_config_hash;
    tbl->handler[HP_MSG_SET_CONFIG] = on_set_config;
    tbl->handler[HP_MSG_COMMAND] = on_command;
    tbl->handler[HP_MSG_PAGE_PUSH] = on_page_push;
}

/* ------------------------------------------------------------------ rf */

static void emit_key(void *vctx, uint8_t remote, uint8_t key, hp_key_action_t action) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "remote", remote);
    cJSON_AddNumberToObject(d, "key", key);
    cJSON_AddNumberToObject(d, "action", action);
    hub_broadcast(vctx, "key_event", d);
}

static void handle_rf_msg(hub_ctx_t *h, const hp_rf_msg_t *m) {
    hp_rf_val_t v[3];
    if (m->type == HP_MSG_KEY_EVENT && hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0) {
        int rc = hp_key_handle(&h->map, &h->sinks, (uint8_t)v[0].u, (uint8_t)v[1].u, (hp_key_action_t)v[2].u,
                               emit_key, h);
        if (rc < 0) fprintf(stderr, "hub: key %u action failed (%d)\n", (unsigned)v[1].u, rc);
    } else if (m->type == HP_MSG_BATTERY && hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0) {
        cJSON *d = cJSON_CreateObject();
        if (!d) return;
        cJSON_AddNumberToObject(d, "remote", v[0].u);
        cJSON_AddNumberToObject(d, "percent", v[1].u);
        cJSON_AddNumberToObject(d, "millivolts", v[2].u);
        hub_broadcast(h, "battery", d);
    } /* anything else from a remote is dropped */
}

void hub_pump_rf(hub_ctx_t *h) {
    if (!h->sinks.rf) return;
    uint8_t fr[HP_RF_FRAME_MAX + 1];
    hp_rf_msg_t msg;
    int n;
    while ((n = h->sinks.rf->recv(h->sinks.rf->self, fr, sizeof fr, 0)) != 0) {
        if (n < 0) { /* link event (remote gone) or error: drop partial reassembly */
            hp_rf_asm_init(&h->rf_asm);
            break;
        }
        if (n > HP_RF_FRAME_MAX) continue;
        if (hp_rf_asm_push(&h->rf_asm, fr, (size_t)n, &msg) == HP_ASM_COMPLETE) handle_rf_msg(h, &msg);
    }
    hub_flush_pending(h); /* a key may have started/stopped an activity */
}
