#include "hub/hub_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/hp_errors.h"

void hub_ctx_init(hub_ctx_t *c) {
    memset(c, 0, sizeof *c);
    hp_map_init(&c->map);
}

/* ------------------------------------------------------------ broadcast */

typedef struct {
    const char *msg;
    size_t len;
} bcast_t;

static void bcast_one(hp_ws_conn_t *c, void *arg) {
    const hp_session_t *s = hp_ws_conn_user(c);
    const bcast_t *b = arg;
    if (s && s->authed) hp_ws_send(c, b->msg, b->len);
}

void hub_broadcast(hub_ctx_t *h, const char *type, cJSON *d) {
    char buf[512];
    size_t n = 0;
    if (h->srv && hp_make_reply(0, type, d, buf, sizeof buf, &n) == 0) {
        bcast_t b = {buf, n};
        hp_ws_foreach(h->srv, bcast_one, &b);
    }
}

void hub_flush_pending(hub_ctx_t *h) {
    if (!h->pending_state) return;
    h->pending_state = false;
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "activity", h->pending_activity);
    cJSON_AddBoolToObject(d, "power", h->pending_power);
    hub_broadcast(h, "state_update", d);
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
    (void)sess;
    hub_ctx_t *h = vctx;
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "config");
    char *text = cJSON_PrintUnformatted(cfg);
    if (!text) return reply_ok(id, "config_ack", false, "out of memory", reply, cap, len);
    int rc = hp_map_load(&h->map, text, strlen(text));
    free(text);
    if (rc < 0) return reply_ok(id, "config_ack", false, load_err(rc), reply, cap, len);
    const char *msg = NULL;
    if (h->cache_path && hp_map_save_cache(&h->map, h->cache_path) < 0) msg = "applied; cache write failed";
    return reply_ok(id, "config_ack", true, msg, reply, cap, len);
}

static int get_u8(const cJSON *d, const char *k, int hi) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)d, k);
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(int)v->valuedouble) return -1;
    int i = (int)v->valuedouble;
    return i >= 0 && i <= hi ? i : -1;
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
        rc = hp_activity_set(&h->map, n->valuestring, &h->sinks);
        if (rc < 0)
            return reply_ok(id, "command_result", false,
                            rc == -HP_E_INVAL ? "unknown activity" : "activity step failed", reply, cap, len);
        h->pending_state = true;
        h->pending_activity = rc;
        h->pending_power = true;
        return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    if (!strcmp(dev, "hub") && !strcmp(cmd, "power_off")) {
        rc = hp_activity_off(&h->map, &h->sinks);
        if (rc < 0)
            return reply_ok(id, "command_result", false,
                            rc == -HP_E_INVAL ? "no activity running" : "off step failed", reply, cap, len);
        h->pending_state = true;
        h->pending_activity = rc;
        h->pending_power = false;
        return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    if (h->sim && !strcmp(dev, "sim") && !strcmp(cmd, "inject_key")) {
        int r = cJSON_IsObject(args) ? get_u8(args, "remote", 255) : -1;
        int k = cJSON_IsObject(args) ? get_u8(args, "key", 255) : -1;
        int a = cJSON_IsObject(args) ? get_u8(args, "action", 2) : -1;
        if (r < 0 || k < 0 || a < 0) return reply_ok(id, "command_result", false, "bad args", reply, cap, len);
        uint8_t fr[3] = {(uint8_t)r, (uint8_t)k, (uint8_t)a};
        rc = hp_sim_rf_script(h->sim, fr, sizeof fr);
        return reply_ok(id, "command_result", rc == 0, rc ? "rf queue full" : NULL, reply, cap, len);
    }
    return reply_ok(id, "command_result", false, "unknown command", reply, cap, len);
}

static cJSON *on_activities(void *vctx) {
    hub_ctx_t *h = vctx;
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    for (size_t i = 0; i < hp_map_activity_count(&h->map); i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(hp_map_activity_name(&h->map, i)));
    return a;
}

void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx) {
    tbl->ctx = ctx;
    tbl->activities = on_activities;
    tbl->handler[HP_MSG_SET_CONFIG] = on_set_config;
    tbl->handler[HP_MSG_COMMAND] = on_command;
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

void hub_pump_rf(hub_ctx_t *h) {
    if (!h->sinks.rf) return;
    uint8_t fr[HP_SIM_RF_MAX];
    int n;
    while ((n = h->sinks.rf->recv(h->sinks.rf->self, fr, sizeof fr, 0)) > 0) {
        if (n != 3 || fr[2] > HP_KEY_ACTION_HOLD) continue; /* malformed: drop */
        int rc = hp_key_handle(&h->map, &h->sinks, fr[0], fr[1], (hp_key_action_t)fr[2], emit_key, h);
        if (rc < 0) fprintf(stderr, "hub: key %u action failed (%d)\n", (unsigned)fr[1], rc);
    }
}
