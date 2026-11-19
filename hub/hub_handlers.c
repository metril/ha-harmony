#define _POSIX_C_SOURCE 200809L
#include "hub/hub_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hub/hp_errors.h"
#include "hub/rf/rf_payload.h"
#include "remote/page/page.h"

/* page_push = 3-byte header + page data, and must fit one RF object body. */
_Static_assert(HP_PAGE_MAX_BYTES + 3 <= HP_RF_BODY_MAX, "page_push no longer fits one RF object");

static uint32_t mono_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void hub_notify(void *vctx, hp_actsm_evt_t evt, int idx) {
    hub_ctx_t *h = vctx;
    if (evt == HP_ACTSM_EVT_DEVICE) {
        /* queue in the order the changes happened (a later change of the same device moves it last) */
        uint8_t n = 0;
        for (uint8_t i = 0; i < h->npending_dev; i++)
            if (h->pending_dev[i] != idx) h->pending_dev[n++] = h->pending_dev[i];
        h->pending_dev[n++] = (uint8_t)idx;
        h->npending_dev = n;
    } else {
        h->pending_state = true;
        h->pending_activity = idx < 0 ? 0 : idx;
        h->pending_power = idx >= 0;
    }
}

void hub_ctx_init(hub_ctx_t *c) {
    memset(c, 0, sizeof *c);
    hp_ota_init(&c->ota, NULL, NULL); /* disabled until main() gives it a directory */
    hp_map_init(&c->map);
    hp_sched_init(&c->sched, mono_ms, NULL);
    hp_irout_init(&c->irout, &c->sched);
    hp_map_set_out(&c->map, &c->irout);
    hp_actsm_init(&c->act, &c->map, &c->sched, &c->sinks, hub_notify, c);
}

void hub_set_clock(hub_ctx_t *c, hp_clock_fn fn, void *ctx) {
    c->sched.clock = fn;
    c->sched.clock_ctx = ctx;
}

static void xfer_drop(hub_ctx_t *h);

int hub_next_timeout_ms(const hub_ctx_t *c) {
    int t = hp_sched_next_timeout_ms(&c->sched);
    if (c->xfer) {
        int32_t left = (int32_t)(c->xfer_deadline - hp_sched_now(&c->sched));
        int r = left > 0 ? (int)(left < 1000000 ? left : 1000000) : 0;
        if (t < 0 || r < t) t = r;
    }
    if (c->link_bound) { /* an in-flight RF object needs a pump at its retry deadline */
        int r = hp_rf_link_next_timeout_ms(&c->link, hp_sched_now(&c->sched));
        if (r >= 0 && (t < 0 || r < t)) t = r;
    }
    return t;
}
void hub_run_timers(hub_ctx_t *c) {
    if (c->xfer && (int32_t)(hp_sched_now(&c->sched) - c->xfer_deadline) >= 0) xfer_drop(c); /* abandoned */
    hp_sched_run(&c->sched, hp_sched_now(&c->sched));
    hub_flush_pending(c); /* a transition may have completed / changed device state */
}

void hub_shutdown(hub_ctx_t *c) {
    hp_cachew_abort(c->xfer);
    c->xfer = NULL;
    if (hp_actsm_flush_state(&c->act) < 0) fprintf(stderr, "hub: final state write failed\n");
}

#define HUB_RF_SLOT 1 /* the single paired remote (docs/hardware/hub-rf-cc2544.md s7) */

static void handle_rf_msg(hub_ctx_t *h, const hp_rf_msg_t *m);
static void link_deliver(void *ctx, const hp_rf_msg_t *m) { handle_rf_msg(ctx, m); }

static void link_result(void *ctx, hp_msg_type_t type, const uint8_t *body, size_t len, bool ok);

/* The link is bound lazily: callers install sinks.rf (and rf_inner_blocks) after hub_ctx_init. */
static void rf_link_ensure(hub_ctx_t *h) {
    if (h->link_bound) return;
    hp_rf_link_init(&h->link, h->sinks.rf, !h->rf_inner_blocks, HUB_RF_SLOT, link_deliver, h);
    h->link.strict = true; /* one downlink frame in the radio at a time (only where TX status exists) */
    if (h->rf_debug) hp_rf_link_set_log(&h->link, hp_rf_log_stderr, NULL);
    h->link.result = link_result;
    h->link.result_ctx = h;
    h->link_bound = true;
}


/* ---- messages held while the remote is away ---- */

static bool is_set_msg(uint8_t t) { return t == HP_MSG_PAGE_PUSH || t == HP_MSG_THEME_PUSH; } /* what a gen's set is made of */
static bool is_tile_msg(uint8_t t) { return t == HP_MSG_TILE_UPDATE || t == HP_MSG_TILE_STATE; }

/* tile_update and tile_state are one key: the later of the two replaces the held one. */
static bool pend_match(const hub_pend_t *p, uint8_t type, uint8_t remote, uint8_t page, uint8_t tile) {
    bool same = p->type == type || (is_tile_msg(p->type) && is_tile_msg(type));
    return same && p->remote == remote && p->page == page && p->tile == tile;
}

static void pend_remove(hub_ctx_t *h, uint8_t i) {
    memmove(&h->pend[i], &h->pend[i + 1], (size_t)(h->npend - i - 1) * sizeof h->pend[0]);
    h->npend--;
}

/* Drops held messages that a newer one makes stale: the same key, and for a page_push every tile_update
 * of that page (the page carries the tile text). */
static void pend_supersede(hub_ctx_t *h, uint8_t type, uint8_t remote, uint8_t page, uint8_t tile) {
    for (uint8_t i = 0; i < h->npend;) {
        const hub_pend_t *p = &h->pend[i];
        bool stale = pend_match(p, type, remote, page, tile) ||
                     (type == HP_MSG_PAGE_PUSH && is_tile_msg(p->type) && p->remote == remote && p->page == page);
        if (stale) pend_remove(h, i);
        else i++;
    }
}

/* A v2 page or theme of a new gen (the config changed) makes every held page and theme of another gen for that remote stale, and the
 * tile messages held with them (they were queued before this push, so they belong to the old config). Tiles queued
 * after an earlier page of the same new gen are untouched: no older-gen page is held by then. */
static void pend_drop_old_gen(hub_ctx_t *h, uint8_t remote, uint8_t gen) {
    bool dropped = false;
    for (uint8_t i = 0; i < h->npend;) {
        const hub_pend_t *p = &h->pend[i];
        if (is_set_msg(p->type) && p->remote == remote && p->has_gen && p->gen != gen) {
            pend_remove(h, i);
            dropped = true;
        } else {
            i++;
        }
    }
    if (!dropped) return;
    for (uint8_t i = 0; i < h->npend;) {
        if (is_tile_msg(h->pend[i].type) && h->pend[i].remote == remote) pend_remove(h, i);
        else i++;
    }
}

/* The held queue is full: tile messages are evicted first, oldest first, so a burst of tile updates never costs a page
 * (an incomplete page set leaves the remote on "Receiving"). With only pages held, a new tile is refused (false) and a new
 * page drops the oldest page. (A page of an older gen, and the page with the same id, are already gone: pend_drop_old_gen,
 * pend_supersede.) Each loss is counted; the first is logged. */
static bool pend_make_room(hub_ctx_t *h, hp_msg_type_t incoming) {
    for (uint8_t i = 0; i < h->npend; i++)
        if (is_tile_msg(h->pend[i].type)) {
            pend_remove(h, i);
            return true;
        }
    if (!h->pend_dropped++) fprintf(stderr, "hub: held queue full of pages; dropping (counted in pend_dropped)\n");
    if (is_tile_msg((uint8_t)incoming)) return false;
    for (uint8_t i = 0; i < h->npend; i++) /* the oldest page; the theme of the set is the last thing to lose */
        if (h->pend[i].type != HP_MSG_THEME_PUSH) {
            pend_remove(h, i);
            return true;
        }
    pend_remove(h, 0);
    return true;
}

static void pend_add(hub_ctx_t *h, const hub_pend_t *src) {
    if (h->npend == HUB_PEND_MAX && !pend_make_room(h, (hp_msg_type_t)src->type)) return; /* a tile that found only pages: dropped */
    h->pend[h->npend++] = *src;
}

static bool rf_up(const hub_ctx_t *h) { return !h->sinks.rf->attached || h->sinks.rf->attached(h->sinks.rf->self); }

/* A page_push finished on the link. Acked: forget its resend count. Abandoned after its RF retries while the remote is
 * still attached: held again and resent after the next frame heard from the remote, at most HUB_PAGE_RESENDS times (then
 * dropped, counted, logged). Not held again when it is stale: a newer push of that page is held or still with the link
 * (it would be overwritten by this older one), or a newer gen has been pushed since. */
static void link_result(void *ctx, hp_msg_type_t type, const uint8_t *body, size_t len, bool ok) {
    hub_ctx_t *h = ctx;
    hp_rf_val_t v[3];
    bool theme = type == HP_MSG_THEME_PUSH;
    if ((type != HP_MSG_PAGE_PUSH && !theme) || hp_rf_unpack(type, body, len, v, 3) != 0 || v[0].u > 255 || v[1].u > 255) return;
    /* a theme_push is keyed (remote, -) like a page_push (remote, page): its own counters, its gen is v[1] */
    uint8_t pg = theme ? 0 : (uint8_t)v[1].u;
    uint8_t *out = theme ? &h->theme_out : &h->page_out[pg], *tries = theme ? &h->theme_tries : &h->page_tries[pg];
    if (*out) (*out)--;
    if (ok) {
        *tries = 0;
        return;
    }
    bool has_gen = theme || (v[2].len >= HP_PAGE_V2_HEADER && v[2].p[0] == 2);
    uint8_t gen = theme ? (uint8_t)v[1].u : has_gen ? v[2].p[5] : 0;
    for (uint8_t i = 0; i < h->npend; i++)
        if (pend_match(&h->pend[i], (uint8_t)type, (uint8_t)v[0].u, pg, 0)) return;
    if (*out || !rf_up(h) || (has_gen && h->has_last_gen && gen != h->last_gen)) return;
    if (*tries >= HUB_PAGE_RESENDS) {
        fprintf(stderr, "hub: %s %u not delivered after %d resends; giving up\n", theme ? "theme" : "page", pg, HUB_PAGE_RESENDS);
        h->pend_dropped++;
        return;
    }
    (*tries)++;
    hub_pend_t n = {.type = (uint8_t)type, .remote = (uint8_t)v[0].u, .page = pg, .has_gen = has_gen, .gen = gen, .wait = true,
                    .rx_mark = h->link.rx_frames, .len = len};
    memcpy(n.payload, body, len);
    pend_add(h, &n);
}

/* ORDER of a set: the theme_push of a generation is delivered (acked, or given up on) before any of its pages. A page of a
 * v2 set is held while a theme_push of that remote is in flight on the link or held (waiting to be resent), so a theme that
 * fails and is resent is not overtaken by pages already queued behind it. Only one generation is ever pushed at a time
 * (a newer one drops the older held messages), so the check does not need the gen. */
static bool theme_pending(const hub_ctx_t *h, uint8_t remote, bool v2_page) {
    if (!v2_page) return false;
    if (h->theme_out) return true;
    for (uint8_t i = 0; i < h->npend; i++)
        if (h->pend[i].type == HP_MSG_THEME_PUSH && h->pend[i].remote == remote) return true;
    return false;
}

/* ORDER of tiles: a tile_update/tile_state of a page must reach the remote AFTER that page (a page arriving drops the
 * tiles it already shows). So a tile is held while its page is held for any reason (a failed delivery waiting to be
 * resent, the set's theme in flight) or while a theme of the remote is in flight or held (the pages wait for it). The
 * link is ordered, so a page already handed to it keeps its tiles behind it. */
static bool tile_blocked(const hub_ctx_t *h, uint8_t remote, uint8_t page) {
    if (h->theme_out) return true;
    for (uint8_t i = 0; i < h->npend; i++) {
        const hub_pend_t *p = &h->pend[i];
        if (p->remote != remote) continue;
        if (p->type == HP_MSG_THEME_PUSH || (p->type == HP_MSG_PAGE_PUSH && p->page == page)) return true;
    }
    return false;
}

/* Packs and queues one RF message on the link (stop-and-wait, driven by hub_pump_rf). While the remote
 * is not attached the message is held instead (latest per key) and sent when the link comes up: that
 * is 0 too. -HP_E_NOENT only without an RF transport; -HP_E_TOOBIG (nothing sent); -HP_E_BUSY (link
 * queue full). A later failure after all retries is counted by the link, not reported here. */
static int rf_send_keyed(hub_ctx_t *h, hp_msg_type_t t, const hp_rf_val_t *v, size_t nv, uint8_t remote, uint8_t page,
                         uint8_t tile) {
    uint8_t payload[HP_RF_MSG_MAX];
    size_t plen = 0;
    if (!h->sinks.rf) return -HP_E_NOENT;
    int rc = hp_rf_pack(t, v, nv, payload, sizeof payload, &plen);
    if (rc < 0) return rc;
    rf_link_ensure(h);
    pend_supersede(h, (uint8_t)t, remote, page, tile);
    bool theme = t == HP_MSG_THEME_PUSH && nv == 3;
    bool has_gen = theme || (t == HP_MSG_PAGE_PUSH && nv == 3 && v[2].len >= HP_PAGE_V2_HEADER && v[2].p[0] == 2);
    uint8_t gen = theme ? (uint8_t)v[1].u : has_gen ? v[2].p[5] : 0;
    if (has_gen) pend_drop_old_gen(h, remote, gen);
    if (t == HP_MSG_PAGE_PUSH || theme) {
        if (theme) h->theme_tries = 0;
        else h->page_tries[page] = 0;
        if (has_gen) {
            h->last_gen = gen;
            h->has_last_gen = true;
        }
    }
    bool set_msg = t == HP_MSG_PAGE_PUSH || theme;
    if (rf_up(h) && !(t == HP_MSG_PAGE_PUSH && theme_pending(h, remote, has_gen)) && /* a page waits for its theme */
        !(is_tile_msg((uint8_t)t) && tile_blocked(h, remote, page))) { /* a tile waits for its page */
        uint8_t *out = t == HP_MSG_PAGE_PUSH ? &h->page_out[page] : theme ? &h->theme_out : NULL;
        if (out) (*out)++; /* counted before the send: the link may resolve it (report) before the call returns */
        rc = hp_rf_link_send(&h->link, t, payload, plen, hp_sched_now(&h->sched));
        if (out && rc != 0 && *out) (*out)--; /* not queued: refused, busy or nobody there */
        if (rc != -HP_E_NOENT && !(rc == -HP_E_BUSY && set_msg)) return rc; /* a full link queue holds a page or theme */
    }
    hub_pend_t n = {.type = (uint8_t)t, .remote = remote, .page = page, .tile = tile, .has_gen = has_gen, .gen = gen,
                    .len = plen};
    memcpy(n.payload, payload, plen);
    pend_add(h, &n);
    return 0;
}

static int rf_send_msg(hub_ctx_t *h, hp_msg_type_t t, const hp_rf_val_t *v, size_t nv) {
    return rf_send_keyed(h, t, v, nv, 0, 0, 0);
}

/* The link is up: send what was held, oldest first, until the link queue is full. */
static void pend_flush(hub_ctx_t *h) {
    for (uint8_t i = 0; i < h->npend && rf_up(h);) {
        const hub_pend_t *p = &h->pend[i];
        if (p->wait && h->link.rx_frames == p->rx_mark) { /* a failed page: after the remote is heard from again */
            i++;
            continue;
        }
        if ((p->type == HP_MSG_PAGE_PUSH && theme_pending(h, p->remote, p->has_gen)) || /* the set's theme goes first */
            (is_tile_msg(p->type) && tile_blocked(h, p->remote, p->page))) { /* and a page before its tiles */
            i++;
            continue;
        }
        uint8_t *out = p->type == HP_MSG_PAGE_PUSH ? &h->page_out[p->page] : p->type == HP_MSG_THEME_PUSH ? &h->theme_out : NULL;
        if (out) (*out)++;
        int rc = hp_rf_link_send(&h->link, (hp_msg_type_t)p->type, p->payload, p->len, hp_sched_now(&h->sched));
        if (out && rc != 0 && *out) (*out)--;
        if (rc == -HP_E_BUSY || rc == -HP_E_NOENT) return; /* later */
        pend_remove(h, i); /* sent, or unsendable */
    }
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
    if (h->bcast_hook && d) h->bcast_hook(h->bcast_ctx, type, d);
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

const hub_info_t *hub_remote_info(const hub_ctx_t *h, uint8_t remote) {
    for (unsigned i = 0; i < HUB_INFO_MAX; i++)
        if (h->info[i].valid && h->info[i].remote == remote) return &h->info[i];
    return NULL;
}

static cJSON *info_json(const hub_info_t *i) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "remote", i->remote);
    cJSON_AddNumberToObject(d, "page_fmt", i->page_fmt);
    cJSON_AddNumberToObject(d, "caps", i->caps);
    cJSON_AddNumberToObject(d, "icons", i->icons);
    cJSON_AddNumberToObject(d, "gen", i->gen);
    cJSON_AddNumberToObject(d, "pages", i->pages);
    return d;
}

void hub_send_state(hub_ctx_t *h, hp_ws_conn_t *c) {
    for (unsigned i = 0; i < HUB_INFO_MAX; i++) { /* what the remote announced, before the state it refers to */
        if (!h->info[i].valid) continue;
        cJSON *o = info_json(&h->info[i]);
        char ib[192];
        size_t il = 0;
        if (o && hp_make_reply(0, "remote_info", o, ib, sizeof ib, &il) == 0) (void)hp_ws_send(c, ib, il);
    }
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
    uint32_t rb = 0;
    char why[96];
    if (hp_ota_rollback_info(&h->ota, &rb, why, sizeof why)) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "state", "rolled_back");
            cJSON_AddNumberToObject(o, "offset", 0);
            cJSON_AddNumberToObject(o, "size", 0);
            cJSON_AddNumberToObject(o, "build", rb);
            cJSON_AddStringToObject(o, "reason", why);
            char ob[320];
            if (hp_make_reply(0, "ota_status", o, ob, sizeof ob, &n) == 0) (void)hp_ws_send(c, ob, n);
        }
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
    if (h->npending_dev) {
        uint8_t q[HP_MAP_MAX_DEVICES], n = h->npending_dev;
        memcpy(q, h->pending_dev, n);
        h->npending_dev = 0;
        for (uint8_t i = 0; i < n; i++)
            if (hp_actsm_device(&h->act, q[i])) hub_broadcast(h, "device_state", device_json(h, q[i]));
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

/* The config_ack of a finished load, and on success the notifications. */
static int ack_loaded(hub_ctx_t *h, hp_session_t *sess, uint32_t id, char *reply, size_t cap, size_t *len) {
    h->pending_list = true;
    h->list_skip = sess;
    hp_actsm_reloaded(&h->act); /* cancels a transition, re-keys device state, queues device_state/state_update */
    /* A running activity is re-reported (to the sender too): a rename keeps its index, so clients
     * that map index -> name would otherwise never learn the new name. */
    int cur = hp_actsm_reported(&h->act);
    if (cur >= 0) {
        h->pending_state = true;
        h->pending_activity = cur;
        h->pending_power = true;
    }
    cJSON *a = cJSON_CreateObject();
    if (!a) return -HP_E_INVAL;
    cJSON_AddBoolToObject(a, "ok", true);
    char hash[9];
    if (hp_map_hash_hex(&h->map, hash)) cJSON_AddStringToObject(a, "config_hash", hash);
    return hp_make_reply(id, "config_ack", a, reply, cap, len);
}

static void xfer_drop(hub_ctx_t *h) {
    hp_cachew_abort(h->xfer);
    h->xfer = NULL;
    h->chunk_total = h->chunk_got = 0;
    h->chunk_next = 0;
    h->chunk_owner = NULL;
}

static void xfer_touch(hub_ctx_t *h) { h->xfer_deadline = hp_sched_now(&h->sched) + HP_XFER_IDLE_MS; }

static int xfer_nack(hub_ctx_t *h, uint32_t id, const char *msg, char *reply, size_t cap, size_t *len) {
    xfer_drop(h);
    return reply_ok(id, "config_ack", false, msg, reply, cap, len);
}

/* Starts receiving `total` bytes of config text into a new cache file (beside the live one). */
static int xfer_begin(hub_ctx_t *h, hp_session_t *sess, size_t total, char *msg, size_t msgcap) {
    xfer_drop(h);
    if (total == 0) {
        snprintf(msg, msgcap, "empty config");
        return -HP_E_INVAL;
    }
    if (total > hp_map_budget(&h->map)) { /* before any size arithmetic or flash use */
        snprintf(msg, msgcap, "config needs ~%zu KiB of hub memory, the hub allows %zu KiB", total / 1024, hp_map_budget(&h->map) / 1024);
        return -HP_E_TOOBIG;
    }
    int rc = hp_cachew_open(&h->xfer, h->cache_path, total);
    if (rc == -HP_E_TOOBIG) {
        size_t fr = hp_cache_free_bytes(h->cache_path);
        snprintf(msg, msgcap, "config needs ~%zu KiB of flash, the hub has %zu KiB free", (total + 1023) / 1024, fr / 1024);
        return rc;
    }
    if (rc < 0) {
        snprintf(msg, msgcap, "out of memory or flash");
        return rc;
    }
    h->chunk_total = total;
    h->chunk_owner = sess;
    xfer_touch(h);
    return 0;
}

/* All the text is in: load it from the file, publish the file as the cache, swap the map in. Any failure
 * (bad config, over budget, out of memory, flash) leaves the previous map AND cache untouched. */
static int xfer_finish(hub_ctx_t *h, hp_session_t *sess, uint32_t id, char *reply, size_t cap, size_t *len) {
    FILE *tf = NULL;
    hp_map_t *fresh = NULL;
    size_t total = h->chunk_total;
    int rc = hp_cachew_finish(h->xfer, &tf);
    const char *msg = NULL;
    if (rc < 0) {
        msg = "could not write the config on the hub (out of flash space?); the previous config is kept";
    } else if ((rc = hp_map_prepare(&h->map, tf, total, &fresh)) < 0) {
        const char *why = hp_map_last_error(&h->map);
        msg = *why ? why : load_err(rc);
    } else if (hp_cachew_publish(h->xfer) < 0) {
        h->xfer = NULL; /* publish frees it either way */
        hp_map_discard(fresh);
        msg = "could not save the config on the hub (out of flash space?); the previous config is kept";
        rc = -HP_E_IO;
    } else {
        h->xfer = NULL;
        hp_map_commit(&h->map, fresh, NULL);
        xfer_drop(h);
        return ack_loaded(h, sess, id, reply, cap, len);
    }
    return xfer_nack(h, id, msg, reply, cap, len);
}

static int on_set_config(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply,
                         size_t cap, size_t *len) {
    hub_ctx_t *h = vctx;
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "config");
    char *text = cJSON_PrintUnformatted(cfg);
    if (!text) return reply_ok(id, "config_ack", false, "out of memory", reply, cap, len);
    size_t n = strlen(text);
    char msg[HP_MAP_ERR_MAX];
    int rc = xfer_begin(h, sess, n, msg, sizeof msg);
    if (rc == 0 && hp_cachew_append(h->xfer, text, n) < 0) {
        snprintf(msg, sizeof msg, "could not write the config on the hub (out of flash space?)");
        rc = -HP_E_IO;
    }
    free(text);
    if (rc < 0) return xfer_nack(h, id, msg, reply, cap, len);
    return xfer_finish(h, sess, id, reply, cap, len);
}

static int on_config_chunk(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                           size_t *len) {
    hub_ctx_t *h = vctx;
    const cJSON *sq = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "seq");
    const cJSON *tt = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "total");
    const cJSON *dt = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "data");
    if (!cJSON_IsNumber(sq) || !cJSON_IsNumber(tt) || !cJSON_IsString(dt))
        return xfer_nack(h, id, "bad config_chunk", reply, cap, len);
    uint32_t seq = (uint32_t)sq->valuedouble, total = (uint32_t)tt->valuedouble;
    size_t n = strlen(dt->valuestring);
    if (n > HP_CFG_CHUNK_BYTES) return xfer_nack(h, id, "config_chunk too large", reply, cap, len);
    if (seq == 0) {
        char msg[HP_MAP_ERR_MAX];
        if (xfer_begin(h, sess, total, msg, sizeof msg) < 0) return xfer_nack(h, id, msg, reply, cap, len);
    } else if (!h->xfer || h->chunk_owner != sess || seq != h->chunk_next || total != h->chunk_total) {
        return xfer_nack(h, id, "config_chunk out of order", reply, cap, len);
    }
    if (h->chunk_got + n > h->chunk_total) return xfer_nack(h, id, "config_chunk longer than its total", reply, cap, len);
    if (hp_cachew_append(h->xfer, dt->valuestring, n) < 0)
        return xfer_nack(h, id, "could not write the config on the hub (out of flash space?); the previous config is kept", reply, cap, len);
    h->chunk_got += n;
    h->chunk_next = seq + 1;
    xfer_touch(h);
    if (h->chunk_got < h->chunk_total) return reply_ok(id, "config_ack", true, NULL, reply, cap, len);
    return xfer_finish(h, sess, id, reply, cap, len);
}

static cJSON *on_limits(void *vctx) {
    const hub_ctx_t *h = vctx;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "budget", (double)hp_map_budget(&h->map));
    cJSON_AddNumberToObject(o, "chunk", HP_CFG_CHUNK_BYTES);
    cJSON_AddNumberToObject(o, "slack_b", HP_MAP_SLACK_BYTES);
    size_t fl = hp_cache_free_bytes(h->cache_path); /* what the cached config may take on flash now (0 = unknown) */
    cJSON_AddNumberToObject(o, "flash_free", fl == SIZE_MAX ? 0 : (double)fl);
    if (fl == 0) cJSON_AddBoolToObject(o, "flash_low", 1); /* 0 alone means "unknown"; with this flag, "none left" */
    cJSON_AddNumberToObject(o, "dev_b", HP_MAP_DEV_BYTES);
    cJSON_AddNumberToObject(o, "cmd_b", HP_MAP_CMD_BYTES);
    cJSON_AddNumberToObject(o, "key_b", HP_MAP_KEY_BYTES);
    cJSON_AddNumberToObject(o, "word_b", HP_MAP_WORD_BYTES);
    cJSON_AddNumberToObject(o, "max_devices", HP_MAP_MAX_DEVICES);
    cJSON_AddNumberToObject(o, "max_activities", HP_MAP_MAX_ACTIVITIES);
    cJSON_AddNumberToObject(o, "max_keys", HP_MAP_MAX_KEYS);
    cJSON_AddNumberToObject(o, "max_steps", HP_MAP_MAX_STEPS);
    cJSON_AddNumberToObject(o, "max_act_devs", HP_MAP_MAX_ACT_DEVS);
    cJSON_AddNumberToObject(o, "max_pronto", HP_MAP_MAX_PRONTO);
    cJSON_AddNumberToObject(o, "name_max", HP_MAP_NAME_MAX);
    return o;
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
        r = reply_ok(id, "command_result", false,
                     rc == -HP_E_BUSY ? "IR line busy, queue full"
                     : h->sinks.ir    ? "IR send failed"
                                      : "no IR output",
                     reply, cap, len);
    } else {
        r = reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    hp_map_free(sc);
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
    uint32_t st = 0; /* optional state: 0 value-only, 1 off, 2 on, 3 unavailable */
    const cJSON *sj = cJSON_GetObjectItemCaseSensitive((cJSON *)args, "state");
    if (sj && (!u8_arg(args, "state", &st) || st > HP_TILE_UNAVAILABLE))
        return reply_ok(id, "command_result", false, "state must be 0..3", reply, cap, len);
    const hub_info_t *ri = hub_remote_info(h, (uint8_t)r);
    hp_rf_val_t v[5] = {{.u = r}, {.u = p}, {.u = t}, {.p = (const uint8_t *)tx->valuestring, .len = strlen(tx->valuestring)}};
    hp_msg_type_t mt = HP_MSG_TILE_UPDATE;
    size_t nv = 4;
    if (ri && (ri->caps & HUB_CAP_TILE_STATE)) { /* tile_state {remote,page,tile,state,text<=31} */
        mt = HP_MSG_TILE_STATE;
        v[3].u = st;
        v[4].p = (const uint8_t *)tx->valuestring;
        v[4].len = hp_text_cut(tx->valuestring, strlen(tx->valuestring), HP_PAGE_MAX_TEXT);
        nv = 5;
    }
    int rc = rf_send_keyed(h, mt, v, nv, (uint8_t)r, (uint8_t)p, (uint8_t)t);
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
                      : rc == -HP_E_BUSY ? (hp_actsm_busy(&h->act) ? "transition in progress"
                                                                   : "IR line busy, queue full")
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
    if (n == (size_t)-1 || n > HP_PAGE_MAX_BYTES) return reply_err(id, "too_big", "page data exceeds 251 bytes", reply, cap, len);
    hp_rf_val_t v[3] = {{.u = (uint32_t)r->valuedouble}, {.u = (uint32_t)pg->valuedouble}, {.p = data, .len = n}};
    int rc = rf_send_keyed(h, HP_MSG_PAGE_PUSH, v, 3, (uint8_t)v[0].u, (uint8_t)v[1].u, 0);
    if (rc == -HP_E_TOOBIG) return reply_err(id, "too_big", "page does not fit the RF budget", reply, cap, len);
    if (rc == -HP_E_NOENT) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    if (rc < 0) return reply_err(id, "rf_error", "RF send failed", reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

/* theme_push {remote, gen, data}: queued exactly like a page_push (superseded by (type, remote), dropped by a newer gen,
 * resent while attached). The 99-byte theme is validated by the remote; the hub only checks that it fits an RF object. */
static int on_theme_push(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap, size_t *len) {
    (void)sess;
    hub_ctx_t *h = vctx;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "remote");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "gen");
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "data");
    uint8_t data[256];
    size_t n = hp_base64_decode(b64->valuestring, strlen(b64->valuestring), data, sizeof data);
    if (n == (size_t)-1 || n > HP_PAGE_MAX_BYTES) return reply_err(id, "too_big", "theme data exceeds 251 bytes", reply, cap, len);
    hp_rf_val_t v[3] = {{.u = (uint32_t)r->valuedouble}, {.u = (uint32_t)g->valuedouble}, {.p = data, .len = n}};
    int rc = rf_send_keyed(h, HP_MSG_THEME_PUSH, v, 3, (uint8_t)v[0].u, 0, 0);
    if (rc == -HP_E_TOOBIG) return reply_err(id, "too_big", "theme does not fit the RF budget", reply, cap, len);
    if (rc == -HP_E_NOENT) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    if (rc < 0) return reply_err(id, "rf_error", "RF send failed", reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static cJSON *on_activities(void *vctx) { return activities_json(vctx); }

static bool on_config_hash(void *vctx, char out[9]) {
    hub_ctx_t *h = vctx;
    return hp_map_hash_hex(&h->map, out);
}

/* ------------------------------------------------------------------ ota */

static int ota_reply(hub_ctx_t *h, uint32_t id, const char *state, const uint32_t *build, char *reply, size_t cap,
                     size_t *len) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return -HP_E_INVAL;
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "offset", h->ota.receiving ? h->ota.offset : 0);
    cJSON_AddNumberToObject(o, "size", h->ota.receiving ? h->ota.size : 0);
    if (build) cJSON_AddNumberToObject(o, "build", *build);
    return hp_make_reply(id, "ota_status", o, reply, cap, len);
}

static int ota_fail(hub_ctx_t *h, uint32_t id, int e, char *reply, size_t cap, size_t *len) {
    return hp_make_error(id, hp_ota_code(e), hp_ota_message(&h->ota, e), reply, cap, len);
}

static double num_of(const cJSON *d, const char *k) { return cJSON_GetObjectItemCaseSensitive((cJSON *)d, k)->valuedouble; }

static int on_ota_begin(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap, size_t *len) {
    hub_ctx_t *h = vctx;
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "sha");
    int e = hp_ota_begin(&h->ota, sess, (unsigned)num_of(d, "target"), (uint32_t)num_of(d, "size"),
                         (uint32_t)num_of(d, "build"), sha->valuestring);
    if (e) return ota_fail(h, id, e, reply, cap, len);
    return ota_reply(h, id, "receiving", NULL, reply, cap, len);
}

static int on_ota_chunk(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap, size_t *len) {
    hub_ctx_t *h = vctx;
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "data");
    static uint8_t buf[HP_OTA_CHUNK_MAX + 8];
    size_t blen = strlen(b64->valuestring);
    if (blen > ((HP_OTA_CHUNK_MAX + 2) / 3) * 4) return ota_fail(h, id, HP_OTA_E_CHUNK, reply, cap, len);
    size_t n = hp_base64_decode(b64->valuestring, blen, buf, sizeof buf);
    if (n == (size_t)-1 || n > HP_OTA_CHUNK_MAX) return ota_fail(h, id, HP_OTA_E_CHUNK, reply, cap, len);
    int e = hp_ota_chunk(&h->ota, sess, (uint32_t)num_of(d, "offset"), buf, n);
    if (e == HP_OTA_E_OFFSET) return hp_make_error(id, hp_ota_code(e), "chunk offset is ahead of the received data", reply, cap, len);
    if (e) return ota_fail(h, id, e, reply, cap, len);
    return ota_reply(h, id, "receiving", NULL, reply, cap, len);
}

static int on_ota_end(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap, size_t *len) {
    (void)d;
    hub_ctx_t *h = vctx;
    uint32_t build = 0;
    int e = hp_ota_end(&h->ota, sess, &build);
    if (e) return ota_fail(h, id, e, reply, cap, len);
    return ota_reply(h, id, "applied", &build, reply, cap, len);
}

static int on_ota_abort(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap, size_t *len) {
    (void)d;
    hub_ctx_t *h = vctx;
    int e = hp_ota_abort(&h->ota, sess);
    if (e) return ota_fail(h, id, e, reply, cap, len);
    return ota_reply(h, id, "aborted", NULL, reply, cap, len);
}

void hub_session_closed(hub_ctx_t *h, const hp_session_t *sess) {
    if (h->xfer && h->chunk_owner == sess) xfer_drop(h); /* its transfer can never finish: free the flash */
    hp_ota_owner_gone(&h->ota, sess);
}

static int on_set_hub_id(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                         size_t *len) {
    (void)sess;
    hub_ctx_t *h = vctx;
    if (!h->ident.id[0]) return reply_err(id, "unsupported", "this hub has no identity store", reply, cap, len);
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "id");
    const cJSON *jn = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "name");
    bool changed = strcmp(jid->valuestring, h->ident.id) != 0;
    int rc = hp_ident_set(&h->ident, jid->valuestring, jn ? jn->valuestring : NULL);
    switch (rc) {
    case HP_IDENT_OK: break;
    case HP_IDENT_BAD_ID: return reply_err(id, "bad_id", "hub id must be 1-63 characters of A-Z a-z 0-9 _ - and not \"default\"", reply, cap, len);
    case HP_IDENT_BAD_NAME: return reply_err(id, "bad_name", "name must be valid UTF-8 without control characters, at most 48 bytes", reply, cap, len);
    case HP_IDENT_PINNED: return reply_err(id, "id_pinned", "the hub id is pinned by hub_id in hub.conf on the device; remove it there to rename from Home Assistant", reply, cap, len);
    default: return reply_err(id, "io", "could not save the hub identity", reply, cap, len);
    }
    if (changed && h->ident_changed) h->ident_changed(h->ident_ctx, &h->ident);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -HP_E_INVAL;
    cJSON_AddStringToObject(o, "hub_id", h->ident.id);
    if (h->ident.name[0]) cJSON_AddStringToObject(o, "name", h->ident.name);
    return hp_make_reply(id, "hub_id_ack", o, reply, cap, len);
}

static void on_hello_extra(void *vctx, cJSON *ack) {
    hub_ctx_t *h = vctx;
    if (h->ident.id[0]) {
        if (h->ident.name[0]) cJSON_AddStringToObject(ack, "name", h->ident.name);
        if (h->ident.hw[0]) cJSON_AddStringToObject(ack, "hw_id", h->ident.hw);
        cJSON_AddBoolToObject(ack, "rename", 1);
        if (h->ident.pinned) cJSON_AddBoolToObject(ack, "id_pinned", 1);
    }
    if (!h->ota.enabled) return;
    cJSON_AddNumberToObject(ack, "fw_build", h->ota.min_build);
    cJSON_AddStringToObject(ack, "ota_key", h->ota.key_dev ? "dev" : "release");
}

void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx) {
    tbl->ctx = ctx;
    tbl->hello_extra = on_hello_extra;
    tbl->activities = on_activities;
    tbl->config_hash = on_config_hash;
    tbl->limits = on_limits;
    tbl->handler[HP_MSG_SET_CONFIG] = on_set_config;
    tbl->handler[HP_MSG_CONFIG_CHUNK] = on_config_chunk;
    tbl->handler[HP_MSG_COMMAND] = on_command;
    tbl->handler[HP_MSG_PAGE_PUSH] = on_page_push;
    tbl->handler[HP_MSG_THEME_PUSH] = on_theme_push;
    tbl->handler[HP_MSG_SET_HUB_ID] = on_set_hub_id;
    tbl->handler[HP_MSG_OTA_BEGIN] = on_ota_begin;
    tbl->handler[HP_MSG_OTA_CHUNK] = on_ota_chunk;
    tbl->handler[HP_MSG_OTA_END] = on_ota_end;
    tbl->handler[HP_MSG_OTA_ABORT] = on_ota_abort;
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

/* A remote link is up. Exactly one `remote_attached` per link-up, and anything cached about the remote is
 * (re)announced after it: Home Assistant drops the info it holds on `remote_attached`. Idempotent: called from
 * every place that can learn of the link first (a frame, the transport's flag), whichever comes first. */
static void rf_link_up(hub_ctx_t *h) {
    if (h->rf_attached) return;
    h->rf_attached = true;
    h->info_fresh = true;
    if (!hp_rf_link_status(&h->link).queued) { /* nothing is in flight: whatever a lost result left counted is void */
        h->theme_out = 0; /* (a strict link keeps its queue across a drop: those objects still report) */
        memset(h->page_out, 0, sizeof h->page_out);
    }
    cJSON *d = cJSON_CreateObject();
    if (d) hub_broadcast(h, "remote_attached", d);
    for (unsigned i = 0; i < HUB_INFO_MAX; i++) {
        if (!h->info[i].valid) continue;
        h->info_fresh = false; /* the cached info is out again: the next identical one is a heartbeat */
        h->info_bcast++;
        cJSON *j = info_json(&h->info[i]);
        if (j) hub_broadcast(h, "remote_info", j);
    }
}

/* The link went away (in sequence with the frames: everything received before is handled). What was in flight
 * and what the old remote announced are void; the next link's frames come after this and are untouched. */
static void rf_link_lost(hub_ctx_t *h) {
    h->rf_attached = false;
    h->info_fresh = true;
    if (!hp_rf_link_status(&h->link).queued) { /* the link dropped what was in flight without a result */
        h->theme_out = 0;
        memset(h->page_out, 0, sizeof h->page_out);
    }
    memset(h->info, 0, sizeof h->info); /* the remote may come back as another firmware: ask again */
}

static void handle_rf_msg(hub_ctx_t *h, const hp_rf_msg_t *m) {
    hp_rf_val_t v[6];
    rf_link_up(h); /* a frame proves the link is up, and its link-up must precede what the frame says */
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
    } else if (m->type == HP_MSG_REMOTE_INFO && hp_rf_unpack(m->type, m->payload, m->len, v, 6) == 0 && v[0].u <= 255) {
        hub_info_t n = {.valid = true, .remote = (uint8_t)v[0].u, .page_fmt = (uint8_t)v[1].u, .caps = (uint8_t)v[2].u,
                        .icons = (uint8_t)v[3].u, .gen = (uint8_t)v[4].u, .pages = (uint8_t)v[5].u};
        hub_info_t *slot = NULL;
        for (unsigned i = 0; i < HUB_INFO_MAX; i++)
            if (h->info[i].valid && h->info[i].remote == n.remote) slot = &h->info[i];
        for (unsigned i = 0; i < HUB_INFO_MAX && !slot; i++)
            if (!h->info[i].valid) slot = &h->info[i];
        if (!slot) slot = &h->info[0]; /* more remotes than slots: the first is recycled */
        bool changed = !slot->valid || slot->remote != n.remote || slot->page_fmt != n.page_fmt || slot->caps != n.caps ||
                       slot->icons != n.icons || slot->gen != n.gen || slot->pages != n.pages;
        *slot = n;
        /* Broadcast on a change, and for the first remote_info after a link-up even when identical (Home Assistant
         * re-pushes its pages after a reattach). The 5-minute heartbeat repeats are not: one broadcast per link-up. */
        if (changed || h->info_fresh) {
            h->info_fresh = false;
            h->info_bcast++;
            cJSON *d = info_json(&n);
            if (d) hub_broadcast(h, "remote_info", d);
        }
    } /* anything else from a remote (unknown types included) is dropped */
}

void hub_pump_rf(hub_ctx_t *h) {
    if (!h->sinks.rf) return;
    rf_link_ensure(h);
    /* Received objects are delivered (and acked) from inside the pump, link loss in sequence with them
     * (-HP_E_NOENT): `remote_attached` is announced by the first frame of a link or by the transport's flag, and
     * `rf_link_up` is idempotent, so it goes out once per link-up and before anything that link says. */
    if (hp_rf_link_pump(&h->link, hp_sched_now(&h->sched)) == -HP_E_NOENT) rf_link_lost(h);
    uint32_t failures = hp_rf_link_status(&h->link).failures;
    if (failures != h->rf_failures) {
        fprintf(stderr, "hub: RF send failed after %d retries (%u object(s) dropped)\n", HP_RF_RETRIES,
                (unsigned)(failures - h->rf_failures));
        h->rf_failures = failures;
    }
    /* A link that is up without having sent a frame yet (or that came back right after a loss handled above). The
     * flag going false is not acted on here: its loss marker is still queued behind the frames of that link. */
    if (h->sinks.rf->attached && h->sinks.rf->attached(h->sinks.rf->self)) rf_link_up(h);
    pend_flush(h);
    hub_flush_pending(h); /* a key may have started/stopped an activity */
}
