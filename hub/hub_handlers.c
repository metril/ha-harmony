#define _POSIX_C_SOURCE 200809L
#include "hub/net/hublog.h"
#include "hub/hub_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "hub/bt_link.h"
#include "hub/hp_errors.h"
#include "hub/net/crash.h"
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
    } else if (evt == HP_ACTSM_EVT_START) {
        static uint8_t hosts[HH_BT_MAX_HOSTS]; /* event-loop thread only */
        int n = hp_map_activity_bt_hosts(&h->map, idx, hosts, (int)sizeof hosts);
        for (int i = 0; i < n; i++) bt_link_connect(hosts[i]); /* the host is connecting while the IR steps run */
    } else {
        h->pending_state = true;
        h->pending_activity = idx < 0 ? 0 : idx;
        h->pending_power = idx >= 0;
    }
}

static void run_queued_ir_send(void *vh, const char *json);

void hub_ctx_init(hub_ctx_t *c) {
    memset(c, 0, sizeof *c);
    hp_ota_init(&c->ota, NULL, NULL); /* disabled until main() gives it a directory */
    hp_map_init(&c->map);
    hp_sched_init(&c->sched, mono_ms, NULL);
    hp_irout_init(&c->irout, &c->sched);
    hp_map_set_out(&c->map, &c->irout);
    hp_actsm_init(&c->act, &c->map, &c->sched, &c->sinks, hub_notify, c);
    hp_actsm_set_irsend(&c->act, run_queued_ir_send, c);
}

void hub_set_clock(hub_ctx_t *c, hp_clock_fn fn, void *ctx) {
    c->sched.clock = fn;
    c->sched.clock_ctx = ctx;
}

static void xfer_drop(hub_ctx_t *h);
static void emit_key(void *vctx, uint8_t remote, uint8_t key, hp_key_action_t action);

/* The loaded config's bt tables go to the link (it runs the helper while there are any, binds hosts by address). */
void hub_bt_config_sync(hub_ctx_t *h) {
    static struct bt_link_cfg_entry hosts[HH_BT_MAX_HOSTS], remotes[HH_BT_MAX_REMOTES]; /* event-loop thread only */
    unsigned nh = 0, nr = 0;
    for (unsigned i = 0; i < h->map.nbt_hosts && nh < HH_BT_MAX_HOSTS; i++) {
        hosts[nh].slot = h->map.bt_hosts[i].slot;
        snprintf(hosts[nh].addr, sizeof hosts[nh].addr, "%s", h->map.bt_hosts[i].has_addr ? h->map.bt_hosts[i].addr : "");
        nh++;
    }
    for (unsigned i = 0; i < h->map.nbt_remotes && nr < HH_BT_MAX_REMOTES; i++) {
        remotes[nr].slot = h->map.bt_remotes[i].slot;
        snprintf(remotes[nr].addr, sizeof remotes[nr].addr, "%s", h->map.bt_remotes[i].has_addr ? h->map.bt_remotes[i].addr : "");
        nr++;
    }
    bt_link_set_config(hosts, nh, remotes, nr);
}

/* What the link needs from the engine: a configured remote's keys, its loss, and the held host key on a helper death. */
static void eng_remote_key(void *vctx, unsigned slot, unsigned page, unsigned usage, bool down) {
    hub_ctx_t *h = vctx;
    (void)hp_map_bt_remote_key(&h->map, &h->sinks, (uint8_t)slot, (uint8_t)page, (uint16_t)usage, down, emit_key, h);
}
static void eng_remote_lost(void *vctx, unsigned slot) {
    hub_ctx_t *h = vctx;
    (void)hp_map_bt_remote_lost(&h->map, &h->sinks, (uint8_t)slot, emit_key, h);
}
static void eng_release_held(void *vctx) {
    hub_ctx_t *h = vctx;
    (void)hp_map_bt_release(&h->map, &h->sinks);
}

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
    /* A silent remote may have been accepted by a transport probe (rf_up) after this iteration's pump looked: nothing
     * else would wake the loop to announce it, so pump again at once (rf_link_up clears this). */
    if (c->sinks.rf && c->sinks.rf->attached && !c->rf_attached && c->sinks.rf->attached(c->sinks.rf->self)) t = 0;
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
    if (hp_actsm_flush_state(&c->act) < 0) hp_log("hub: final state write failed\n");
}

#define HUB_RF_SLOT 1 /* the single paired remote (docs/hardware/hub-rf-cc2544.md s7) */

static void handle_rf_msg(hub_ctx_t *h, const hp_rf_msg_t *m);
static void link_deliver(void *ctx, const hp_rf_msg_t *m) { handle_rf_msg(ctx, m); }

static void link_result(void *ctx, hp_msg_type_t type, const uint8_t *body, size_t len, bool ok);
static void cover_failed(hub_ctx_t *h, uint8_t remote, uint32_t cid);

static void rf_log(void *c, const char *s) {
    (void)c;
    hp_log("%s", s);
}

/* The link is bound lazily: callers install sinks.rf (and rf_inner_blocks) after hub_ctx_init. */
static void rf_link_ensure(hub_ctx_t *h) {
    if (h->link_bound) return;
    hp_rf_link_init(&h->link, h->sinks.rf, !h->rf_inner_blocks, HUB_RF_SLOT, link_deliver, h);
    h->link.strict = true; /* one downlink frame in the radio at a time (only where TX status exists) */
    if (h->rf_debug) hp_rf_link_set_log(&h->link, rf_log, NULL); /* via hp_log: the lines ship to HA too */
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
    if (!h->pend_dropped++) hp_log("hub: held queue full of pages; dropping (counted in pend_dropped)\n");
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
    hp_rf_val_t v[4];
    if (type == HP_MSG_COVER_CHUNK) {
        if (!ok && hp_rf_unpack(type, body, len, v, 4) == 0) cover_failed(h, (uint8_t)v[0].u, v[1].u);
        return;
    }
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
        hp_log("hub: %s %u not delivered after %d resends; giving up\n", theme ? "theme" : "page", pg, HUB_PAGE_RESENDS);
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

/* ---- Now Playing: the cover and now_playing holds ---- */

static bool remote_awake(const hub_ctx_t *h) {
    return h->link.rx_frames && (int32_t)(hp_sched_now(&h->sched) - h->link.last_rx) < HUB_AWAKE_MS;
}

/* One shipped line per kind at most every 2 s (the first always): a playing track resends often, the log must not flood. */
static bool np_log_ok(hub_ctx_t *h, unsigned kind) {
    uint32_t now = hp_sched_now(&h->sched);
    if (h->np_log_at[kind] && (int32_t)(now - h->np_log_at[kind]) < 2000) return false;
    h->np_log_at[kind] = now ? now : 1;
    return true;
}

static hub_cover_t *cover_slot(hub_ctx_t *h, uint8_t remote) {
    for (unsigned i = 0; i < HUB_COVER_MAX; i++)
        if (h->cover[i].used && h->cover[i].remote == remote) return &h->cover[i];
    for (unsigned i = 0; i < HUB_COVER_MAX; i++)
        if (!h->cover[i].used) return &h->cover[i];
    return &h->cover[0]; /* more remotes than slots: the first is recycled */
}

static hub_np_t *np_slot(hub_ctx_t *h, uint8_t remote) {
    for (unsigned i = 0; i < HUB_NP_MAX; i++)
        if (h->np[i].used && h->np[i].remote == remote) return &h->np[i];
    for (unsigned i = 0; i < HUB_NP_MAX; i++)
        if (!h->np[i].used) return &h->np[i];
    return &h->np[0];
}

static bool cover_was_sent(const hub_cover_t *c, uint32_t cid) {
    for (unsigned i = 0; i < c->nsent; i++)
        if (c->sent[i] == cid) return true;
    return false;
}

static void cover_forget(hub_cover_t *c, uint32_t cid) {
    for (unsigned i = 0; i < c->nsent; i++)
        if (c->sent[i] == cid) {
            c->sent[i] = c->sent[--c->nsent];
            c->sent_next = (uint8_t)(c->nsent % HUB_COVER_SENT);
            return;
        }
}

static void cover_mark_sent(hub_cover_t *c, uint32_t cid) {
    if (cover_was_sent(c, cid)) return;
    if (c->nsent < HUB_COVER_SENT) c->sent[c->nsent++] = cid;
    else c->sent[c->sent_next] = cid;
    c->sent_next = (uint8_t)((c->sent_next + 1) % HUB_COVER_SENT);
}

/* A chunk was abandoned after its RF retries: the remote's copy is incomplete. Sent again (from the start), a few times. */
static void cover_failed(hub_ctx_t *h, uint8_t remote, uint32_t cid) {
    hub_cover_t *c = cover_slot(h, remote);
    if (!c->used || c->remote != remote || c->cid != cid) return;
    cover_forget(c, cid);
    if (c->tries >= HUB_PAGE_RESENDS) {
        hp_log("hub: cover %08x not delivered after %d resends; giving up\n", (unsigned)cid, HUB_PAGE_RESENDS);
        c->hold = false;
        return;
    }
    c->tries++;
    c->hold = true;
    c->off = 0;
}

/* Hands the held covers and now_playing messages to the link: never while the remote is not heard (asleep: it cannot be
 * woken, and each frame would cost the link's 300 ms timeout). Covers first; a full link queue (-HP_E_BUSY) waits. */
static void cover_np_flush(hub_ctx_t *h) {
    if (!h->sinks.rf || !rf_up(h) || !remote_awake(h)) return;
    for (unsigned i = 0; i < HUB_COVER_MAX; i++) {
        hub_cover_t *c = &h->cover[i];
        while (c->used && c->hold) {
            size_t n = HUB_COVER_BYTES - c->off < HUB_COVER_CHUNK ? HUB_COVER_BYTES - c->off : HUB_COVER_CHUNK;
            hp_rf_val_t v[4] = {{.u = c->remote}, {.u = c->cid}, {.u = c->off}, {.p = c->data + c->off, .len = n}};
            uint8_t pl[HP_RF_MSG_MAX];
            size_t pn = 0;
            if (hp_rf_pack(HP_MSG_COVER_CHUNK, v, 4, pl, sizeof pl, &pn) < 0) {
                c->hold = false;
                break;
            }
            int rc = hp_rf_link_send(&h->link, HP_MSG_COVER_CHUNK, pl, pn, hp_sched_now(&h->sched));
            if (rc == -HP_E_BUSY || rc == -HP_E_NOENT) return; /* later */
            c->off = (uint16_t)(c->off + n);
            if (c->off >= HUB_COVER_BYTES) {
                c->hold = false;
                cover_mark_sent(c, c->cid);
                if (np_log_ok(h, 3)) hp_log("hub: cover %08x sent over RF to remote %u\n", (unsigned)c->cid, (unsigned)c->remote);
            }
        }
    }
    for (unsigned i = 0; i < HUB_NP_MAX; i++) {
        hub_np_t *p = &h->np[i];
        if (!p->used || !p->hold) continue;
        uint32_t pos = p->pos;
        if (p->flags & 1) { /* playing: the position moved on while the message was held */
            pos += (hp_sched_now(&h->sched) - p->at) / 1000;
            if (p->dur && pos > p->dur) pos = p->dur;
            if (pos > 0xFFFF) pos = 0xFFFF;
        }
        hp_rf_val_t v[7] = {{.u = p->remote}, {.u = p->cid}, {.u = p->flags}, {.u = pos}, {.u = p->dur},
                            {.p = (const uint8_t *)p->t1, .len = p->l1}, {.p = (const uint8_t *)p->t2, .len = p->l2}};
        uint8_t pl[HP_RF_MSG_MAX];
        size_t pn = 0;
        if (hp_rf_pack(HP_MSG_NOW_PLAYING, v, 7, pl, sizeof pl, &pn) < 0) {
            p->hold = false;
            continue;
        }
        int rc = hp_rf_link_send(&h->link, HP_MSG_NOW_PLAYING, pl, pn, hp_sched_now(&h->sched));
        if (rc == -HP_E_BUSY || rc == -HP_E_NOENT) return;
        p->hold = false;
        if (np_log_ok(h, 1))
            hp_log("hub: now_playing sent over RF to remote %u (cid %08x flags %u pos %u)\n", (unsigned)p->remote,
                   (unsigned)p->cid, (unsigned)p->flags, (unsigned)pos);
    }
}

/* ------------------------------------------------------------ broadcast */

typedef struct {
    const char *msg;
    size_t len;
    const hp_session_t *skip;
    bool bt_listeners_only; /* bt_remote_key: only sockets that sent bt_listen on */
} bcast_t;

static void bcast_one(hp_ws_conn_t *c, void *arg) {
    const hp_session_t *s = hp_ws_conn_user(c);
    const bcast_t *b = arg;
    if (s && s->authed && s != b->skip && (!b->bt_listeners_only || s->bt_listen)) hp_ws_send(c, b->msg, b->len);
}

static void broadcast_ex(hub_ctx_t *h, const char *type, cJSON *d, const hp_session_t *skip, bool bt_only) {
    static char buf[HP_WS_MAX_FRAME]; /* one frame; event-loop thread only. bt_state with 255 hosts needs ~30 KB */
    size_t n = 0;
    if (h->bcast_hook && d) h->bcast_hook(h->bcast_ctx, type, d);
    if (h->srv && hp_make_reply(0, type, d, buf, sizeof buf, &n) == 0) {
        bcast_t b = {buf, n, skip, bt_only};
        hp_ws_foreach(h->srv, bcast_one, &b);
    } else if (!h->srv) {
        cJSON_Delete(d); /* hp_make_reply not reached: we still own d */
    }
}

static void broadcast_except(hub_ctx_t *h, const char *type, cJSON *d, const hp_session_t *skip) {
    broadcast_ex(h, type, d, skip, false);
}

void hub_broadcast(hub_ctx_t *h, const char *type, cJSON *d) { broadcast_ex(h, type, d, NULL, false); }

static void count_sinks(hp_ws_conn_t *c, void *arg) {
    const hp_session_t *s = hp_ws_conn_user(c);
    if (s && s->authed && s->log_sink) ++*(int *)arg;
}

typedef struct {
    const char *msg;
    size_t len;
    int delivered;
} sinkmsg_t;

static void send_sink(hp_ws_conn_t *c, void *arg) {
    const hp_session_t *s = hp_ws_conn_user(c);
    sinkmsg_t *b = arg;
    if (s && s->authed && s->log_sink && hp_ws_send(c, b->msg, b->len) >= 0) b->delivered++;
}

bool hub_log_emit(void *hub, const hp_logline_t *l) {
    hub_ctx_t *h = hub;
    int n = 0;
    if (!h->srv) return false;
    hp_ws_foreach(h->srv, count_sinks, &n);
    if (!n) return false;
    cJSON *d = cJSON_CreateObject();
    if (!d) return false;
    cJSON_AddStringToObject(d, "level", l->level);
    cJSON_AddStringToObject(d, "msg", l->msg);
    cJSON_AddNumberToObject(d, "ts", l->ts);
    if (l->dropped) cJSON_AddNumberToObject(d, "dropped", l->dropped);
    static char buf[1024]; /* event-loop thread only */
    size_t len = 0;
    if (h->bcast_hook) h->bcast_hook(h->bcast_ctx, "hub_log", d);
    if (hp_make_reply(0, "hub_log", d, buf, sizeof buf, &len) != 0) return true; /* d is consumed; cannot happen at 192 B */
    sinkmsg_t b = {buf, len, 0};
    hp_ws_foreach(h->srv, send_sink, &b);
    return b.delivered > 0; /* every send failed: the line stays queued for the next sink */
}

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

static cJSON *bt_state_json(void);

int hub_bt_state_frame(char *buf, size_t cap, size_t *len) {
    if (!bt_link_available()) return -HP_E_NOENT;
    cJSON *o = bt_state_json();
    return o ? hp_make_reply(0, "bt_state", o, buf, cap, len) : -HP_E_INVAL;
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
    { /* a read-only way to learn the Bluetooth state: it follows every hello (only on builds with Bluetooth) */
        static char bb[HP_WS_MAX_FRAME];
        size_t bn = 0;
        if (hub_bt_state_frame(bb, sizeof bb, &bn) == 0) (void)hp_ws_send(c, bb, bn);
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
    if (h->bt_dirty) { /* one bt_state per event-loop pass, however many changes the link reported */
        h->bt_dirty = false;
        cJSON *d = bt_state_json();
        if (d) hub_broadcast(h, "bt_state", d);
    }
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
    hub_bt_config_sync(h);
    hp_actsm_reloaded(&h->act); /* no-op for an unchanged config; else re-keys device state, restarts a running plan, queues device_state/state_update */
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

/* Parses and sends an ir_send action (deferred while the IR line is busy). NULL ok, else the reply text. */
static const char *do_ir_send(hub_ctx_t *h, const cJSON *args, char *err, size_t errn) {
    hp_map_t *sc = calloc(1, sizeof *sc); /* scratch pool for the action's IR data */
    if (!sc) return "out of memory";
    hp_map_set_out(sc, &h->irout);
    hp_action_t a;
    const char *why = NULL;
    int rc = hp_map_parse_ir(sc, args, &a);
    if (rc < 0) {
        snprintf(err, errn, "%s", sc->err[0] ? sc->err : "invalid IR action"); /* sc is freed below */
        why = err;
    } else if ((rc = hp_map_apply_deferrable(sc, &a, &h->sinks)) < 0) {
        why = rc == -HP_E_BUSY ? "IR line busy, queue full" : h->sinks.ir ? "IR send failed" : "no IR output";
    }
    if (why) hp_log("hub: ir_send failed: %s\n", why);
    hp_map_free(sc);
    free(sc);
    return why;
}

/* A queued ir_send (from a transition) is run when the transition finishes. */
static void run_queued_ir_send(void *vh, const char *json) {
    cJSON *args = cJSON_Parse(json);
    char err[HP_MAP_ERR_MAX];
    if (args) (void)do_ir_send(vh, args, err, sizeof err);
    cJSON_Delete(args);
}

static int cmd_ir_send(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    if (!cJSON_IsObject(args)) return reply_ok(id, "command_result", false, "args must be an IR action", reply, cap, len);
    if (hp_actsm_busy(&h->act)) { /* queued: it runs when the transition finishes */
        char *txt = cJSON_PrintUnformatted(args);
        if (!txt) return reply_ok(id, "command_result", false, "out of memory", reply, cap, len);
        if (hp_actsm_defer_irsend(&h->act, txt) < 0) {
            free(txt);
            return reply_ok(id, "command_result", false, "transition in progress, queue full", reply, cap, len);
        }
        return reply_ok(id, "command_result", true, NULL, reply, cap, len);
    }
    char err[HP_MAP_ERR_MAX];
    const char *why = do_ir_send(h, args, err, sizeof err);
    return reply_ok(id, "command_result", why == NULL, why, reply, cap, len);
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

static bool uint_arg(const cJSON *o, const char *k, double max, uint32_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > max || v->valuedouble != (double)(uint32_t)v->valuedouble)
        return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}

/* now_playing {remote, cid, flags, pos, dur, t1, t2}: held (latest per remote) and sent when the remote is awake. */
static int cmd_now_playing(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    uint32_t r, cid, fl, pos, dur;
    const cJSON *t1 = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "t1") : NULL;
    const cJSON *t2 = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "t2") : NULL;
    if (!cJSON_IsObject(args) || !uint_arg(args, "remote", 255, &r) || !uint_arg(args, "cid", 4294967295.0, &cid) ||
        !uint_arg(args, "flags", 255, &fl) || !uint_arg(args, "pos", 65535, &pos) || !uint_arg(args, "dur", 65535, &dur) ||
        !cJSON_IsString(t1) || !cJSON_IsString(t2))
        return reply_ok(id, "command_result", false, "args need remote, cid, flags, pos, dur, t1 and t2", reply, cap, len);
    if (!h->sinks.rf) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    rf_link_ensure(h);
    hub_np_t *p = np_slot(h, (uint8_t)r);
    memset(p, 0, sizeof *p);
    p->used = p->hold = true;
    p->remote = (uint8_t)r;
    p->cid = cid;
    p->flags = (uint8_t)fl;
    p->pos = (uint16_t)pos;
    p->dur = (uint16_t)dur;
    p->at = hp_sched_now(&h->sched);
    p->l1 = (uint8_t)hp_text_cut(t1->valuestring, strlen(t1->valuestring), HUB_NP_TEXT);
    p->l2 = (uint8_t)hp_text_cut(t2->valuestring, strlen(t2->valuestring), HUB_NP_TEXT);
    memcpy(p->t1, t1->valuestring, p->l1);
    memcpy(p->t2, t2->valuestring, p->l2);
    if (np_log_ok(h, 0))
        hp_log("hub: now_playing held for remote %u (cid %08x flags %u awake %d)\n", (unsigned)r, (unsigned)cid, (unsigned)fl,
               (int)remote_awake(h));
    cover_np_flush(h);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

/* cover {remote, cid, data = base64 of HUB_COVER_BYTES}: one slot per remote, a newer cid replaces it (even half sent). A
 * cid already sent to that remote is not sent again until it rejoins. Pages are never touched. */
static int cmd_cover(hub_ctx_t *h, uint32_t id, const cJSON *args, char *reply, size_t cap, size_t *len) {
    uint32_t r, cid;
    const cJSON *b64 = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive((cJSON *)args, "data") : NULL;
    if (!cJSON_IsObject(args) || !uint_arg(args, "remote", 255, &r) || !uint_arg(args, "cid", 4294967295.0, &cid) ||
        !cJSON_IsString(b64))
        return reply_ok(id, "command_result", false, "args need remote, cid and data", reply, cap, len);
    if (!h->sinks.rf) return reply_err(id, "no_remote", "no remote attached", reply, cap, len);
    rf_link_ensure(h);
    hub_cover_t *c = cover_slot(h, (uint8_t)r);
    if (c->used && c->remote == r && ((c->hold && c->cid == cid) || cover_was_sent(c, cid)))
        return reply_ok(id, "command_result", true, NULL, reply, cap, len); /* already held or sent */
    uint8_t buf[HUB_COVER_BYTES + 4];
    size_t n = hp_base64_decode(b64->valuestring, strlen(b64->valuestring), buf, sizeof buf);
    if (n != HUB_COVER_BYTES) return reply_ok(id, "command_result", false, "data must be 1760 bytes", reply, cap, len);
    if (!c->used || c->remote != r) {
        memset(c, 0, sizeof *c);
        c->used = true;
        c->remote = (uint8_t)r;
    }
    c->cid = cid;
    c->off = 0;
    c->tries = 0;
    c->hold = true;
    memcpy(c->data, buf, n);
    if (np_log_ok(h, 2)) hp_log("hub: cover %08x held for remote %u (awake %d)\n", (unsigned)cid, (unsigned)r, (int)remote_awake(h));
    cover_np_flush(h);
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
                      : rc == -HP_E_BUSY ? (hp_actsm_busy(&h->act) ? "transition in progress, queue full"
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
    hp_crumb("cmd %.24s %.32s", dev, cmd);
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
    if (!strcmp(dev, "hub") && !strcmp(cmd, "now_playing")) return cmd_now_playing(h, id, args, reply, cap, len);
    if (!strcmp(dev, "hub") && !strcmp(cmd, "cover")) return cmd_cover(h, id, args, reply, cap, len);
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
    int e = hp_ota_end_start(&h->ota, sess);
    if (e == HP_OTA_ASYNC) { /* the worker runs off the loop; the reply is sent by hub_ota_complete */
        h->ota_end_pend = true;
        h->ota_end_sess = sess;
        h->ota_end_id = id;
        *len = 0;
        return 0;
    }
    return ota_fail(h, id, e, reply, cap, len);
}

typedef struct { const void *sess; hp_ws_conn_t *found; } find_conn_t;
static void find_conn_cb(hp_ws_conn_t *c, void *arg) {
    find_conn_t *f = arg;
    if (hp_ws_conn_user(c) == f->sess) f->found = c;
}

void hub_ota_complete(hub_ctx_t *h, hp_ws_server_t *srv) {
    if (!h->ota_end_pend) return;
    uint32_t build = 0;
    const void *who = NULL;
    bool alive = false;
    int e = hp_ota_end_done(&h->ota, &build, &who, &alive);
    if (e == HP_OTA_E_STATE) return; /* not finished yet */
    h->ota_end_pend = false;
    if (!alive) return; /* the asking connection is gone: the update is applied (or not) regardless */
    find_conn_t f = {h->ota_end_sess, NULL};
    hp_ws_foreach(srv, find_conn_cb, &f);
    if (!f.found) return;
    char reply[512];
    size_t len = 0;
    int r = e ? ota_fail(h, h->ota_end_id, e, reply, sizeof reply, &len)
              : ota_reply(h, h->ota_end_id, "applied", &build, reply, sizeof reply, &len);
    if (r == 0 && len) (void)hp_ws_send(f.found, reply, len);
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

/* ------------------------------------------------------------ bluetooth (docs/proto/bluetooth.md) */

static const char *const bt_status_name[] = {"off", "starting", "ready", "no_adapter", "disabled"};
static const char *const bt_result_name[] = {NULL, "ok", "failed", "refused"};
static const char *const bt_transport_name[] = {NULL, "classic", "le"};
#define BT_NELEM(a) (sizeof(a) / sizeof((a)[0]))

/* Everything the helper reports is untrusted: strictly AA:BB:CC:DD:EE:FF (hex digits, any case). */
static bool bt_addr_ok(const char *a) {
    if (strnlen(a, BT_LINK_ADDR_MAX) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (a[i] != ':') return false;
        } else if (!((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f') || (a[i] >= 'A' && a[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* A name from a nearby device: forced NUL-terminated by the caller, kept only when it is valid UTF-8. */
static bool bt_name_ok(const char *n) { return n[0] && hp_utf8_valid((const uint8_t *)n, strlen(n)); }

/* Bytes a string takes inside JSON quotes (cJSON escapes quote, backslash and control bytes). */
static size_t json_len(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += (unsigned char)*s < 0x20 ? 6 : (*s == '"' || *s == '\\') ? 2 : 1;
    return n;
}

#define BT_STATE_BUDGET 40000 /* of the 64 KiB frame: hosts + remotes beyond it lose their (optional) names */
#define BT_ENTRY_FIXED 135    /* (addr is validated to 17 chars, so this is a true bound) */ /* slot, addr, transport, connected and punctuation of one entry, upper bound */

static cJSON *bt_state_json(void) {
    static struct bt_link_state st; /* ~20 KB: event-loop thread only, not on the stack */
    size_t used = 0;
    bt_link_state(&st);
    for (unsigned i = 0; i < BT_LINK_HOSTS_CAP; i++) {
        st.hosts[i].addr[BT_LINK_ADDR_MAX - 1] = 0;
        st.hosts[i].name[BT_LINK_NAME_MAX - 1] = 0;
    }
    for (unsigned i = 0; i < BT_LINK_REMOTES_CAP; i++) {
        st.remotes[i].addr[BT_LINK_ADDR_MAX - 1] = 0;
        st.remotes[i].name[BT_LINK_NAME_MAX - 1] = 0;
    }
    st.pairing_addr[BT_LINK_ADDR_MAX - 1] = 0;
    st.pairing_name[BT_LINK_NAME_MAX - 1] = 0;
    cJSON *o = cJSON_CreateObject();
    cJSON *hosts = cJSON_CreateArray();
    if (!o || !hosts) {
        cJSON_Delete(o);
        cJSON_Delete(hosts);
        return NULL;
    }
    cJSON_AddStringToObject(o, "state", (unsigned)st.status < BT_NELEM(bt_status_name) ? bt_status_name[st.status] : "off");
    cJSON_AddNumberToObject(o, "gen", st.gen);
    cJSON_AddBoolToObject(o, "discoverable", st.discoverable);
    if (st.discoverable) cJSON_AddNumberToObject(o, "discoverable_secs", st.discoverable_secs);
    if (st.window == BT_WIN_HOST || st.window == BT_WIN_REMOTE)
        cJSON_AddStringToObject(o, "window", st.window == BT_WIN_REMOTE ? "remote" : "host");
    cJSON_AddBoolToObject(o, "le", st.le);
    if ((unsigned)st.pairing > BT_PAIR_NONE && (unsigned)st.pairing < BT_NELEM(bt_result_name) && bt_addr_ok(st.pairing_addr)) {
        cJSON *p = cJSON_AddObjectToObject(o, "pairing");
        if (p) {
            cJSON_AddStringToObject(p, "result", bt_result_name[st.pairing]);
            cJSON_AddStringToObject(p, "addr", st.pairing_addr);
            if (bt_name_ok(st.pairing_name)) cJSON_AddStringToObject(p, "name", st.pairing_name);
            cJSON_AddStringToObject(p, "kind", st.pairing_kind == BT_KIND_REMOTE ? "remote" : "host");
            if ((unsigned)st.pairing_transport > BT_TRANSPORT_ANY && (unsigned)st.pairing_transport < BT_NELEM(bt_transport_name))
                cJSON_AddStringToObject(p, "transport", bt_transport_name[st.pairing_transport]);
        }
    }
    for (unsigned i = 0; i < st.n_hosts && i < BT_LINK_HOSTS_CAP; i++) {
        if (!bt_addr_ok(st.hosts[i].addr)) continue;
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddNumberToObject(e, "slot", st.hosts[i].slot);
        cJSON_AddStringToObject(e, "addr", st.hosts[i].addr);
        used += BT_ENTRY_FIXED;
        if (bt_name_ok(st.hosts[i].name) && used + json_len(st.hosts[i].name) + 10 <= BT_STATE_BUDGET) {
            cJSON_AddStringToObject(e, "name", st.hosts[i].name);
            used += json_len(st.hosts[i].name) + 10;
        }
        cJSON_AddStringToObject(e, "transport",
                                st.hosts[i].transport == BT_TRANSPORT_LE ? "le" : "classic");
        cJSON_AddBoolToObject(e, "connected", st.hosts[i].connected);
        cJSON_AddBoolToObject(e, "configured", st.hosts[i].configured);
        cJSON_AddItemToArray(hosts, e);
    }
    cJSON_AddItemToObject(o, "hosts", hosts);
    cJSON *remotes = cJSON_AddArrayToObject(o, "remotes");
    for (unsigned i = 0; remotes && i < st.n_remotes && i < BT_LINK_REMOTES_CAP; i++) {
        if (!bt_addr_ok(st.remotes[i].addr)) continue;
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddNumberToObject(e, "slot", st.remotes[i].slot);
        cJSON_AddStringToObject(e, "addr", st.remotes[i].addr);
        used += BT_ENTRY_FIXED;
        if (bt_name_ok(st.remotes[i].name) && used + json_len(st.remotes[i].name) + 10 <= BT_STATE_BUDGET) {
            cJSON_AddStringToObject(e, "name", st.remotes[i].name);
            used += json_len(st.remotes[i].name) + 10;
        }
        cJSON_AddBoolToObject(e, "connected", st.remotes[i].connected);
        cJSON_AddBoolToObject(e, "configured", st.remotes[i].configured);
        if (st.remotes[i].battery >= 0) cJSON_AddNumberToObject(e, "battery", st.remotes[i].battery);
        cJSON_AddItemToArray(remotes, e);
    }
    cJSON *cands = st.n_candidates ? cJSON_AddArrayToObject(o, "candidates") : NULL;
    for (unsigned i = 0; cands && i < st.n_candidates && i < BT_LINK_CANDIDATES_CAP; i++) {
        st.candidates[i].addr[BT_LINK_ADDR_MAX - 1] = 0;
        st.candidates[i].name[BT_LINK_NAME_MAX - 1] = 0;
        if (!bt_addr_ok(st.candidates[i].addr)) continue;
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "addr", st.candidates[i].addr);
        if (bt_name_ok(st.candidates[i].name)) cJSON_AddStringToObject(e, "name", st.candidates[i].name);
        cJSON_AddNumberToObject(e, "rssi", st.candidates[i].rssi);
        cJSON_AddItemToArray(cands, e);
    }
    return o;
}

/* The link's listener: any state change goes to every authenticated client. */
static void on_bt_changed(void *vctx) { ((hub_ctx_t *)vctx)->bt_dirty = true; } /* coalesced: see hub_flush_pending */

/* A key from a bonded LE remote: only to sockets that opted in with bt_listen. */
static void on_bt_remote_key(void *vctx, unsigned slot, unsigned page, unsigned usage, bool down) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "slot", slot);
    cJSON_AddNumberToObject(d, "page", page);
    cJSON_AddNumberToObject(d, "usage", usage);
    cJSON_AddBoolToObject(d, "down", down);
    broadcast_ex(vctx, "bt_remote_key", d, NULL, true);
}

static int bt_reply_state(uint32_t id, char *reply, size_t cap, size_t *len) {
    cJSON *d = bt_state_json();
    if (!d) return -HP_E_INVAL;
    return hp_make_reply(id, "bt_state", d, reply, cap, len);
}

static int bt_err(uint32_t id, int rc, char *reply, size_t cap, size_t *len) {
    switch (rc) {
    case BT_LINK_E_NOT_READY: return reply_err(id, "bt_not_ready", "Bluetooth is not ready", reply, cap, len);
    case BT_LINK_E_NO_SLOT: return reply_err(id, "no_slot", "no paired host in that slot", reply, cap, len);
    case BT_LINK_E_NOT_CONNECTED: return reply_err(id, "not_connected", "that host is not connected", reply, cap, len);
    default: return reply_err(id, "bt_busy", "Bluetooth is busy, try again", reply, cap, len);
    }
}

#define BT_GATE(id, lim)                                                                  \
    struct bt_link_limits lim;                                                            \
    if (!bt_link_available()) return reply_err(id, "unsupported", "this hub has no Bluetooth", reply, cap, len); \
    bt_link_limits(&lim)

static int on_bt_pair(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                      size_t *len) {
    (void)vctx;
    (void)sess;
    BT_GATE(id, lim);
    const cJSON *on = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "on");
    const cJSON *js = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "secs");
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "transport");
    const cJSON *jm = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "mode");
    unsigned secs = 120;
    enum bt_link_transport t = BT_TRANSPORT_ANY;
    enum bt_link_pair_mode mode = BT_MODE_HOST;
    if (js) {
        if (js->valuedouble < 1 || js->valuedouble > 300) return reply_err(id, "bad_secs", "secs must be 1-300", reply, cap, len);
        secs = (unsigned)js->valueint;
    }
    if (jt) {
        if (!strcmp(jt->valuestring, "classic")) t = BT_TRANSPORT_CLASSIC;
        else if (!strcmp(jt->valuestring, "le")) t = BT_TRANSPORT_LE;
        else if (strcmp(jt->valuestring, "any")) return reply_err(id, "bad_transport", "transport must be any, classic or le", reply, cap, len);
        if (t == BT_TRANSPORT_LE && !lim.le) return reply_err(id, "no_le", "this hub's adapter has no Bluetooth LE", reply, cap, len);
    }
    if (jm) {
        if (!strcmp(jm->valuestring, "remote")) mode = BT_MODE_REMOTE;
        else if (strcmp(jm->valuestring, "host")) return reply_err(id, "bad_mode", "mode must be host or remote", reply, cap, len);
    }
    if (mode == BT_MODE_REMOTE) {
        if (t == BT_TRANSPORT_CLASSIC) return reply_err(id, "bad_transport", "a remote pairs over le", reply, cap, len);
        if (!lim.remotes_max) return reply_err(id, "unsupported", "this hub does not support Bluetooth remotes", reply, cap, len);
        if (!lim.le) return reply_err(id, "no_le", "this hub's adapter has no Bluetooth LE", reply, cap, len);
    }
    const cJSON *ja = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "addr");
    if (ja) { /* a pick: only "remote, on" takes an address, and it must be a listed candidate */
        if (mode != BT_MODE_REMOTE || !cJSON_IsTrue(on) || !bt_addr_ok(ja->valuestring))
            return reply_err(id, "bad_addr", "addr must be AA:BB:CC:DD:EE:FF with mode remote and on", reply, cap, len);
        static struct bt_link_state st; /* event-loop thread only */
        bt_link_state(&st);
        const char *pick = NULL;
        for (unsigned i = 0; i < st.n_candidates && i < BT_LINK_CANDIDATES_CAP; i++) {
            st.candidates[i].addr[BT_LINK_ADDR_MAX - 1] = 0;
            if (bt_addr_ok(st.candidates[i].addr) && !strcasecmp(st.candidates[i].addr, ja->valuestring)) {
                pick = st.candidates[i].addr;
                break;
            }
        }
        if (!pick) return reply_err(id, "no_candidate", "that device is not in the candidate list", reply, cap, len);
        int prc = bt_link_pair_remote(pick);
        if (prc != BT_LINK_OK) return bt_err(id, prc, reply, cap, len);
        return bt_reply_state(id, reply, cap, len);
    }
    int rc = bt_link_pair(cJSON_IsTrue(on), secs, t, mode);
    if (rc != BT_LINK_OK) return bt_err(id, rc, reply, cap, len);
    return bt_reply_state(id, reply, cap, len);
}

static int on_bt_forget(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                        size_t *len) {
    (void)vctx;
    (void)sess;
    BT_GATE(id, lim);
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "slot");
    const cJSON *jk = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "kind");
    enum bt_link_kind kind = BT_KIND_HOST;
    if (jk) {
        if (!strcmp(jk->valuestring, "remote")) kind = BT_KIND_REMOTE;
        else if (strcmp(jk->valuestring, "host")) return reply_err(id, "bad_kind", "kind must be host or remote", reply, cap, len);
    }
    if (slot->valueint >= (kind == BT_KIND_REMOTE ? lim.remotes_max : lim.hosts_max))
        return reply_err(id, "no_slot", "no paired device in that slot", reply, cap, len);
    int rc = bt_link_forget(kind, (unsigned)slot->valueint);
    if (rc != BT_LINK_OK) return bt_err(id, rc, reply, cap, len);
    return bt_reply_state(id, reply, cap, len);
}

static int on_bt_listen(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                        size_t *len) {
    (void)vctx;
    if (!bt_link_available()) return reply_err(id, "unsupported", "this hub has no Bluetooth", reply, cap, len);
    sess->bt_listen = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)d, "on"));
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static int on_bt_key(void *vctx, hp_session_t *sess, uint32_t id, const cJSON *d, char *reply, size_t cap,
                     size_t *len) {
    (void)vctx;
    (void)sess;
    BT_GATE(id, lim);
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "slot");
    const cJSON *page = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "page");
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "usage");
    const cJSON *act = cJSON_GetObjectItemCaseSensitive((cJSON *)d, "action");
    enum bt_link_action a;
    if (!strcmp(act->valuestring, "down")) a = BT_KEY_DOWN;
    else if (!strcmp(act->valuestring, "up")) a = BT_KEY_UP;
    else if (!strcmp(act->valuestring, "press")) a = BT_KEY_PRESS;
    else return reply_err(id, "bad_action", "action must be down, up or press", reply, cap, len);
    if (page->valueint != BT_PAGE_KEYBOARD && page->valueint != BT_PAGE_CONSUMER)
        return reply_err(id, "bad_page", "page must be 7 or 12", reply, cap, len);
    if (slot->valueint >= lim.hosts_max) return reply_err(id, "no_slot", "no paired host in that slot", reply, cap, len);
    int rc = bt_link_key((unsigned)slot->valueint, (unsigned)page->valueint, (unsigned)usage->valueint, a);
    if (rc != BT_LINK_OK) return bt_err(id, rc, reply, cap, len);
    return reply_ok(id, "command_result", true, NULL, reply, cap, len);
}

static void on_hello_extra(void *vctx, cJSON *ack) {
    hub_ctx_t *h = vctx;
    if (h->ident.id[0]) {
        if (h->ident.name[0]) cJSON_AddStringToObject(ack, "name", h->ident.name);
        if (h->ident.hw[0]) cJSON_AddStringToObject(ack, "hw_id", h->ident.hw);
        cJSON_AddBoolToObject(ack, "rename", 1);
        if (h->ident.pinned) cJSON_AddBoolToObject(ack, "id_pinned", 1);
    }
    if (bt_link_available()) {
        struct bt_link_limits lim;
        bt_link_limits(&lim);
        cJSON *bt = cJSON_AddObjectToObject(ack, "bt");
        cJSON *pg = cJSON_CreateArray();
        if (bt && pg) {
            cJSON_AddNumberToObject(bt, "hosts_max", lim.hosts_max);
            cJSON_AddNumberToObject(bt, "conn_max", lim.conn_max);
            cJSON_AddNumberToObject(bt, "remotes_max", lim.remotes_max);
            cJSON_AddNumberToObject(bt, "le_bonds_max", lim.le_bonds_max);
            cJSON_AddItemToArray(pg, cJSON_CreateNumber(BT_PAGE_KEYBOARD));
            cJSON_AddItemToArray(pg, cJSON_CreateNumber(BT_PAGE_CONSUMER));
            cJSON_AddItemToObject(bt, "pages", pg);
            cJSON_AddBoolToObject(bt, "le", lim.le);
        } else {
            cJSON_Delete(pg);
        }
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
    tbl->handler[HP_MSG_BT_PAIR] = on_bt_pair;
    tbl->handler[HP_MSG_BT_FORGET] = on_bt_forget;
    tbl->handler[HP_MSG_BT_KEY] = on_bt_key;
    tbl->handler[HP_MSG_BT_LISTEN] = on_bt_listen;
    bt_link_set_remote_key_cb(on_bt_remote_key, ctx);
    static struct bt_link_engine eng; /* one hub context per process */
    eng = (struct bt_link_engine){ctx, eng_remote_key, eng_remote_lost, eng_release_held};
    bt_link_set_engine(&eng);
    bt_link_set_listener(on_bt_changed, ctx);
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
    int layer = ((hub_ctx_t *)vctx)->map.key_layer;
    cJSON_AddNumberToObject(d, "activity", layer >= 0 && layer < 255 ? layer : 255);
    hub_broadcast(vctx, "key_event", d);
}

/* A remote link is up. Exactly one `remote_attached` per link-up, and anything cached about the remote is
 * (re)announced after it: Home Assistant drops the info it holds on `remote_attached`. Idempotent: called from
 * every place that can learn of the link first (a frame, the transport's flag), whichever comes first. */
/* A re-up this soon after a loss, whose first remote_info shows the same generation, is a link flap: the remote
 * still holds everything it had (same gen and a non-empty page set), so nothing is announced or resent for it. */
#define HUB_FLAP_MS 10000

/* What a (re)attached remote needs again: its covers and now-playing resent, and the announcement clients redo
 * their pushes on. `remote` < 0 = not known yet (nothing on this hub ever named a remote). */
static void rf_attach_work(hub_ctx_t *h, int remote) {
    for (unsigned i = 0; i < HUB_COVER_MAX; i++) { /* the remote may have rebooted and lost its covers: send again */
        hub_cover_t *c = &h->cover[i];
        c->nsent = c->sent_next = c->tries = 0;
        c->off = 0;
        c->hold = c->used;
    }
    for (unsigned i = 0; i < HUB_NP_MAX; i++) h->np[i].hold = h->np[i].used;
    cJSON *d = cJSON_CreateObject();
    if (d && remote >= 0) cJSON_AddNumberToObject(d, "remote", remote);
    if (d) hub_broadcast(h, "remote_attached", d);
}

/* `remote`: the remote id the frame that revealed the link names, -1 when none did (the transport's flag). */
static void rf_link_up(hub_ctx_t *h, int remote) {
    if (remote >= 0) {
        h->last_remote = (uint8_t)remote;
        h->last_remote_valid = true;
    } else if (h->last_remote_valid) {
        remote = h->last_remote;
    }
    if (h->rf_attached) return;
    h->rf_attached = true;
    hp_log("hub: rf link up\n");
    h->info_fresh = true;
    bool flap = false;
    if (h->lost_valid && (int32_t)(hp_sched_now(&h->sched) - h->lost_at) <= HUB_FLAP_MS)
        for (unsigned i = 0; i < HUB_INFO_MAX; i++) flap = flap || h->lost_info[i].valid;
    h->lost_valid = false;
    h->attach_deferred = flap;
    if (flap) hp_log("hub: rf link re-up after a flap: attach waits for remote_info\n");
    if (!flap) rf_attach_work(h, remote);
    if (!hp_rf_link_status(&h->link).queued) { /* nothing is in flight: whatever a lost result left counted is void */
        h->theme_out = 0; /* (a strict link keeps its queue across a drop: those objects still report) */
        memset(h->page_out, 0, sizeof h->page_out);
    }
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
    hp_log("hub: rf link lost\n");
    h->info_fresh = true;
    if (!hp_rf_link_status(&h->link).queued) { /* the link dropped what was in flight without a result */
        h->theme_out = 0;
        memset(h->page_out, 0, sizeof h->page_out);
    }
    if (h->attach_deferred) { /* lost again before it said anything: the older loss still stands */
        h->attach_deferred = false;
        h->lost_valid = true;
    } else {
        memcpy(h->lost_info, h->info, sizeof h->info);
        h->lost_at = hp_sched_now(&h->sched);
        h->lost_valid = true;
    }
    memset(h->info, 0, sizeof h->info); /* the remote may come back as another firmware: ask again */
}

/* A press/hold that sat in the receive queue (main loop stalled) longer than this is no longer what the user wants:
 * it is dropped, never executed. A release is always processed (it ends a hold). */
#define HUB_KEY_STALE_MS 1500

/* The frame being delivered was stamped by the transport's reader when it arrived (same monotonic ms clock as the
 * hub's). True (and logged) when it is older than HUB_KEY_STALE_MS; a transport without stamps never drops. */
static bool key_stale(hub_ctx_t *h, unsigned key) {
    uint32_t rx_at, tx_at;
    if (!h->sinks.rf || !h->sinks.rf->frame_times || !h->sinks.rf->frame_times(h->sinks.rf->self, &rx_at, &tx_at)) return false;
    int32_t age = (int32_t)(hp_sched_now(&h->sched) - rx_at);
    if (age <= HUB_KEY_STALE_MS) return false;
    if (np_log_ok(h, 5)) hp_log("hub: key %u dropped: stale (%d ms old)\n", key, (int)age);
    return true;
}

/* The remote id a frame names (key_event, battery, remote_info lead with it), -1 for any other. */
static int rf_msg_remote(const hp_rf_msg_t *m) {
    hp_rf_val_t v[6];
    size_t n = m->type == HP_MSG_REMOTE_INFO ? 6 : 3;
    if (m->type != HP_MSG_KEY_EVENT && m->type != HP_MSG_BATTERY && m->type != HP_MSG_REMOTE_INFO) return -1;
    if (hp_rf_unpack(m->type, m->payload, m->len, v, n) != 0 || v[0].u > 255) return -1;
    return (int)v[0].u;
}

static void handle_rf_msg(hub_ctx_t *h, const hp_rf_msg_t *m) {
    hp_rf_val_t v[6];
    rf_link_up(h, rf_msg_remote(m)); /* a frame proves the link is up, and its link-up must precede what the frame says */
    if (m->type == HP_MSG_KEY_EVENT && hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0) {
        if (v[2].u != HP_KEY_ACTION_RELEASE && key_stale(h, (unsigned)v[1].u)) return;
        /* Every press/release is logged (seq = the remote's link seq, to match its log); a held key repeats, so those are rate limited. */
        if (v[2].u != HP_KEY_ACTION_HOLD || np_log_ok(h, 4))
            hp_log("hub: rf key remote %u key %u action %u seq %u\n", (unsigned)v[0].u, (unsigned)v[1].u, (unsigned)v[2].u,
                   (unsigned)m->seq);
        int rc = hp_key_handle(&h->map, &h->sinks, (uint8_t)v[0].u, (uint8_t)v[1].u, (hp_key_action_t)v[2].u,
                               emit_key, h);
        if (rc < 0) hp_log("hub: key %u action failed (%d)\n", (unsigned)v[1].u, rc);
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
        if (h->attach_deferred) { /* the first word of a flap re-up: the same generation = nothing was lost */
            h->attach_deferred = false;
            bool same = false;
            for (unsigned i = 0; i < HUB_INFO_MAX; i++)
                same = same || (h->lost_info[i].valid && h->lost_info[i].remote == n.remote && h->lost_info[i].gen == n.gen &&
                                h->lost_info[i].pages == n.pages && n.pages > 0); /* a remote holding nothing is always served */
            if (!same) rf_attach_work(h, n.remote);
        }
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
        hp_log("hub: RF send failed after %d retries (%u object(s) dropped)\n", HP_RF_RETRIES,
                (unsigned)(failures - h->rf_failures));
        h->rf_failures = failures;
    }
    /* A link that is up without having sent a frame yet (or that came back right after a loss handled above). The
     * flag going false is not acted on here: its loss marker is still queued behind the frames of that link. */
    if (h->sinks.rf->attached && h->sinks.rf->attached(h->sinks.rf->self)) rf_link_up(h, -1);
    pend_flush(h);
    cover_np_flush(h);
    hub_flush_pending(h); /* a key may have started/stopped an activity */
}
