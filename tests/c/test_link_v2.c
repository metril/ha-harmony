/* Page format v2 over the fake radio (tests/c/fake_radio.h): remote_info caching, tile_state vs tile_update
 * selection and the shared held-message key, held pages of an older gen, and the page store filling from 40 pages
 * held while the remote was away. Real hub context and real hp_rf_link_t ends. */
#include "fake_radio.h"
#include "hub/net/ws.h"
#include "remote/page/page.h"
#include "remote/page/store.h"

#define LOG_MAX 512
static hp_rf_msg_t rlog[LOG_MAX];
static int nlog;
static void log_msg(const hp_rf_msg_t *m) {
    if (nlog < LOG_MAX) rlog[nlog++] = *m;
}
static void reset(void) { nlog = 0; }

static void world(void) {
    remote_hook = log_msg;
    world_reset_hook = reset;
    world_init(true, MODEL_S3);
    run(400);
    CT_CHECK(R.up);
}

static void send_info_stored(uint32_t caps, uint32_t gen, uint32_t pages) {
    hp_rf_val_t v[6] = {{.u = 1}, {.u = 2}, {.u = caps}, {.u = 50}, {.u = gen}, {.u = pages}};
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    CT_EQ(hp_rf_pack(HP_MSG_REMOTE_INFO, v, 6, pl, sizeof pl, &n), 0);
    CT_EQ(hp_rf_link_send(&RL, HP_MSG_REMOTE_INFO, pl, n, R.now), 0);
    run(300);
}
static void send_info(uint32_t caps) { send_info_stored(caps, 0, 0); }

/* A v2 page: id of total, gen, one button. Returns the raw length. */
static size_t mkpage(uint8_t *o, uint8_t id, uint8_t total, uint8_t gen, uint8_t kind, uint8_t act) {
    const uint8_t b[] = {2, id, 2, 2, 1, gen, total, kind, 0, act, 0, 0, 1, 0x00, 0x11, 1, 'X'};
    memcpy(o, b, sizeof b);
    return sizeof b;
}

static int push_raw(uint8_t id, const uint8_t *raw, size_t n) {
    char b64[400];
    hp_base64_encode(raw, n, b64);
    return push_page(id, b64);
}

static int tile_cmd(uint8_t page, uint8_t tile, const char *text, int state) {
    char js[256];
    if (state >= 0)
        snprintf(js, sizeof js, "{\"device\":\"hub\",\"command\":\"tile_update\",\"args\":{\"remote\":1,\"page\":%u,\"tile\":%u,\"text\":\"%s\",\"state\":%d}}", page, tile, text, state);
    else
        snprintf(js, sizeof js, "{\"device\":\"hub\",\"command\":\"tile_update\",\"args\":{\"remote\":1,\"page\":%u,\"tile\":%u,\"text\":\"%s\"}}", page, tile, text);
    cJSON *d = cJSON_Parse(js);
    char reply[512];
    size_t len = 0;
    int rc = TBL.handler[HP_MSG_COMMAND](&H, NULL, 9, d, reply, sizeof reply, &len);
    cJSON_Delete(d);
    reply[len < sizeof reply ? len : sizeof reply - 1] = 0;
    return rc == 0 && strstr(reply, "\"ok\":true");
}

static void test_remote_info_cached_and_cleared_on_link_loss(void) {
    world();
    CT_CHECK(hub_remote_info(&H, 1) == NULL);
    send_info(3);
    const hub_info_t *i = hub_remote_info(&H, 1);
    CT_CHECK(i != NULL);
    if (i) {
        CT_EQ(i->page_fmt, 2);
        CT_EQ(i->caps, 3);
        CT_EQ(i->icons, 50);
        CT_EQ(i->gen, 0);
        CT_EQ(i->pages, 0);
    }
    send_info_stored(3, 0xA7, 12); /* what the remote's page store holds is cached with the rest */
    CT_CHECK(hub_remote_info(&H, 1) && hub_remote_info(&H, 1)->gen == 0xA7 && hub_remote_info(&H, 1)->pages == 12);
    send_info(1); /* a later one replaces it */
    CT_CHECK(hub_remote_info(&H, 1) && hub_remote_info(&H, 1)->caps == 1);
    CT_CHECK(hub_remote_info(&H, 2) == NULL);
    hp_rf_link_set_awake(&RL, false, R.now); /* the remote goes away: link lost */
    run(900);
    CT_CHECK(!R.up);
    CT_CHECK(hub_remote_info(&H, 1) == NULL); /* what it said is stale */
    hp_rf_link_set_awake(&RL, true, R.now);
    run(600);
    send_info(3); /* and it says it again as its first uplink after the link-up */
    CT_CHECK(hub_remote_info(&H, 1) && hub_remote_info(&H, 1)->caps == 3);
}

static void test_unknown_and_unexpected_types_are_dropped(void) {
    world();
    hp_rf_val_t v[5] = {{.u = 1}, {.u = 0}, {.u = 0}, {.u = 2}, {.p = (const uint8_t *)"x", .len = 1}};
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    CT_EQ(hp_rf_pack(HP_MSG_TILE_STATE, v, 5, pl, sizeof pl, &n), 0); /* a hub-bound tile_state is nonsense: dropped */
    CT_EQ(hp_rf_link_send(&RL, HP_MSG_TILE_STATE, pl, n, R.now), 0);
    run(300);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* acked, ignored */
    CT_EQ(hp_rf_link_status(&H.link).delivered, 1u);
    send_info(3);
    CT_CHECK(hub_remote_info(&H, 1) != NULL); /* the link still works */
}

static const hp_rf_msg_t *last_of(uint8_t type) {
    for (int i = nlog - 1; i >= 0; i--)
        if (rlog[i].type == type) return &rlog[i];
    return NULL;
}

static void test_tile_state_only_for_remotes_that_announce_it(void) {
    world();
    CT_CHECK(tile_cmd(0, 3, "21 C", 2)); /* no remote_info: legacy tile_update, state ignored */
    run(300);
    CT_CHECK(last_of(HP_MSG_TILE_UPDATE) && !last_of(HP_MSG_TILE_STATE));
    send_info(2); /* page store only: still no tile_state */
    CT_CHECK(tile_cmd(0, 3, "22 C", 2));
    run(300);
    CT_CHECK(!last_of(HP_MSG_TILE_STATE));
    send_info(3);
    CT_CHECK(tile_cmd(0, 3, "On", 2));
    run(300);
    const hp_rf_msg_t *m = last_of(HP_MSG_TILE_STATE);
    CT_CHECK(m != NULL);
    if (m) {
        hp_rf_val_t v[5];
        CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, m->payload, m->len, v, 5), 0);
        CT_EQ(v[1].u, 0);
        CT_EQ(v[2].u, 3);
        CT_EQ(v[3].u, 2);
        CT_EQ(v[4].len, 2u);
    }
    CT_CHECK(tile_cmd(0, 4, "v", -1)); /* no state arg: value-only (0) */
    run(300);
    m = last_of(HP_MSG_TILE_STATE);
    hp_rf_val_t v[5];
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, m->payload, m->len, v, 5), 0);
    CT_EQ(v[3].u, 0);
    CT_CHECK(!tile_cmd(0, 4, "v", 4)); /* state > 3 refused */
    char big[100];
    memset(big, 'a', 99);
    memcpy(big + 29, "\xc3\xa9", 2); /* 'é' straddles byte 30..31: the cut must not split it */
    big[99] = 0;
    CT_CHECK(tile_cmd(0, 5, big, 1));
    run(300);
    m = last_of(HP_MSG_TILE_STATE);
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, m->payload, m->len, v, 5), 0);
    CT_CHECK(v[4].len <= HP_PAGE_MAX_TEXT && hp_text_cut((const char *)v[4].p, v[4].len, 99) == v[4].len);
}

static void test_tile_update_and_state_share_one_held_key(void) {
    world();
    send_info(3);
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    CT_CHECK(hub_remote_info(&H, 1) == NULL); /* cleared by the link loss: plain tile_updates; then the remote announces */
    CT_CHECK(tile_cmd(2, 7, "a", -1));
    CT_EQ(H.npend, 1);
    CT_CHECK(tile_cmd(2, 7, "b", 1)); /* still legacy (no info): same key */
    CT_EQ(H.npend, 1);
    H.info[0] = (hub_info_t){.valid = true, .remote = 1, .page_fmt = 2, .caps = 3, .icons = 50}; /* the remote announced */
    CT_CHECK(tile_cmd(2, 7, "c", 2)); /* tile_state replaces the held tile_update */
    CT_EQ(H.npend, 1);
    CT_EQ(H.pend[0].type, HP_MSG_TILE_STATE);
    CT_CHECK(tile_cmd(2, 8, "d", 2));
    CT_EQ(H.npend, 2);
    uint8_t raw[40];
    size_t n = mkpage(raw, 2, 3, 9, 0, 0);
    CT_CHECK(push_raw(2, raw, n)); /* page_push supersedes the tiles of that page */
    CT_EQ(H.npend, 1);
    CT_EQ(H.pend[0].type, HP_MSG_PAGE_PUSH);
}

static int count_pend(uint8_t type) {
    int n = 0;
    for (int i = 0; i < H.npend; i++) n += H.pend[i].type == type;
    return n;
}

static void test_newer_gen_drops_held_pages_of_older_gen(void) {
    world();
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    for (uint8_t id = 0; id < 5; id++) CT_CHECK(push_raw(id, raw, mkpage(raw, id, 5, 0x11, 0, 0)));
    CT_CHECK(tile_cmd(1, 1, "old", -1));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 5);
    CT_EQ(count_pend(HP_MSG_TILE_UPDATE), 1);
    CT_CHECK(push_raw(0, raw, mkpage(raw, 0, 3, 0x22, 1, 0))); /* a new config: gen 0x22 */
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 1); /* the five old pages and their tile are gone */
    CT_EQ(count_pend(HP_MSG_TILE_UPDATE), 0);
    CT_CHECK(tile_cmd(0, 1, "new", -1));
    CT_CHECK(push_raw(1, raw, mkpage(raw, 1, 3, 0x22, 0, 0))); /* same gen: nothing else dropped */
    CT_CHECK(push_raw(2, raw, mkpage(raw, 2, 3, 0x22, 0, 0)));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 3);
    CT_EQ(count_pend(HP_MSG_TILE_UPDATE), 1); /* page 0's tile came after page 0 of the new gen: kept */
    /* v1 pages carry no gen: never dropped by one */
    const uint8_t v1[] = {1, 9, 1, 1, 0};
    CT_CHECK(push_raw(9, v1, sizeof v1));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 4);
}

/* A full set (HP_STORE_MAX pages) plus tiles, held while the
 * remote is away, is all delivered, in order, when it comes back, and the page store fills from them. */
static hp_store_t ST;
static uint8_t order[256];
static int norder;
static void store_hook(const hp_rf_msg_t *m) {
    hp_rf_val_t v[3];
    if (m->type != HP_MSG_PAGE_PUSH || hp_rf_unpack(m->type, m->payload, m->len, v, 3) != 0) return;
    if (norder < 256) order[norder++] = (uint8_t)v[1].u;
    CT_EQ(hp_store_put(&ST, v[2].p, v[2].len), HP_STORE_OK);
}
static void store_reset(void) { norder = 0; hp_store_init(&ST); }

static void test_max_held_pages_fill_the_store_in_order(void) {
    remote_hook = store_hook;
    world_reset_hook = store_reset;
    world_init(true, MODEL_S3);
    run(400);
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    for (unsigned id = 0; id < HP_STORE_MAX; id++) CT_CHECK(push_raw((uint8_t)id, raw, mkpage(raw, (uint8_t)id, HP_STORE_MAX, 0x55, id == 3 ? 1 : 0, 0)));
    for (uint8_t t = 0; t < 2; t++) CT_CHECK(tile_cmd((uint8_t)(t + 1), 1, "t", -1));
    CT_EQ(H.npend, HP_STORE_MAX + 2); /* every page of a full set and the tiles fit HUB_PEND_MAX: nothing was pushed out */
    CT_CHECK(H.npend <= HUB_PEND_MAX);
    run(300);
    CT_EQ(norder, 0);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(20000);
    CT_EQ(norder, HP_STORE_MAX);
    for (int i = 0; i < norder; i++) CT_EQ(order[i], i); /* in order */
    CT_EQ(H.npend, 0);
    CT_EQ(hp_store_count(&ST), HP_STORE_MAX);
    CT_CHECK(hp_store_complete(&ST));
    CT_EQ(hp_store_find_kind(&ST, HP_PAGE_KIND_HOME), 3);
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
    CT_CHECK(R.max_depth <= 1);
}

/* A burst of tile updates while the remote is away must never push a page out of the held queue. */
static void test_tile_burst_never_evicts_pages(void) {
    remote_hook = store_hook;
    world_reset_hook = store_reset;
    world_init(true, MODEL_S3);
    run(400);
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    for (uint8_t id = 0; id < 10; id++) CT_CHECK(push_raw(id, raw, mkpage(raw, id, 10, 0x66, id == 2 ? 1 : 0, 0)));
    for (int t = 0; t < 300; t++) CT_CHECK(tile_cmd((uint8_t)(t / 30), (uint8_t)(t % 30), "v", -1));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 10);
    CT_EQ(H.npend, HUB_PEND_MAX);
    /* the newest tiles are the ones kept */
    CT_CHECK(H.pend[H.npend - 1].type == HP_MSG_TILE_UPDATE && H.pend[H.npend - 1].page == 9 && H.pend[H.npend - 1].tile == 29);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(20000);
    CT_EQ(norder, 10);
    for (int i = 0; i < norder; i++) CT_EQ(order[i], i);
    CT_CHECK(hp_store_complete(&ST));
}

/* Only pages held and the queue full: a new tile is refused, a new page drops the oldest page (counted). */
static void test_full_queue_of_pages_refuses_tiles_and_drops_oldest_page(void) {
    world();
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    const uint8_t v1[] = {1, 9, 1, 1, 0}; /* v1 pages: no gen, so no older-gen purge */
    for (uint8_t id = 0; id < HUB_PEND_MAX; id++) { /* ids 0..254; 255 is a fresh key */
        uint8_t b[sizeof v1];
        memcpy(b, v1, sizeof v1);
        b[1] = id;
        CT_CHECK(push_raw(id, b, sizeof b));
    }
    CT_EQ(H.npend, HUB_PEND_MAX);
    CT_CHECK(tile_cmd(1, 1, "x", -1)); /* dropped: still ok to the caller, no page lost */
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), HUB_PEND_MAX);
    CT_EQ(H.pend_dropped, 1u);
    CT_CHECK(push_raw(255, raw, mkpage(raw, 0, 1, 0x70, 0, 0)));
    CT_EQ(H.npend, HUB_PEND_MAX);
    CT_EQ(H.pend[0].page, 1); /* page 0 was the oldest */
    CT_EQ(H.pend_dropped, 2u);
}

/* A page_push that exhausts its RF retries while the remote stays attached is held and resent: not silently lost. */
static void test_failed_page_push_is_resent_while_attached(void) {
    remote_hook = store_hook;
    world_reset_hook = store_reset;
    world_init(true, MODEL_S3);
    run(400);
    CT_CHECK(R.up);
    uint8_t raw[40];
    R.drop_dl = 1000; /* everything the hub transmits is lost on the air */
    CT_CHECK(push_raw(0, raw, mkpage(raw, 0, 1, 0x31, 0, 0)));
    run(3000);
    CT_CHECK(hp_rf_link_status(&H.link).failures >= 1u);
    CT_EQ(norder, 0);
    CT_CHECK(R.up); /* the remote never went away */
    R.drop_dl = 0;
    run(3000);
    CT_EQ(norder, 1);
    CT_CHECK(hp_store_complete(&ST));
    CT_EQ(H.npend, 0);
}

/* Bounded: a remote that never answers gets HUB_PAGE_RESENDS resends, then the page is dropped (counted). */
static void test_page_resends_are_bounded(void) {
    world();
    uint8_t raw[40];
    R.drop_dl = 100000;
    CT_CHECK(push_raw(0, raw, mkpage(raw, 0, 1, 0x32, 0, 0)));
    run(20000);
    CT_EQ(hp_rf_link_status(&H.link).failures, 1u + HUB_PAGE_RESENDS);
    CT_EQ(H.npend, 0);
    CT_CHECK(H.pend_dropped >= 1u);
    /* a newer gen supersedes the failed older page: it is not resent */
    R.drop_dl = 1000;
    CT_CHECK(push_raw(0, raw, mkpage(raw, 0, 1, 0x33, 0, 0)));
    run(500);
    R.drop_dl = 100000;
    CT_CHECK(push_raw(1, raw, mkpage(raw, 0, 1, 0x34, 0, 0))); /* gen 0x34 on the air; 0x33 is older */
    run(20000);
    CT_EQ(H.npend, 0);
}

/* remote_info: identical heartbeats are not broadcast; the first one after a seen link-up is, once. */
static void test_remote_info_broadcast_once_per_link_up(void) {
    world();
    send_info(3);
    CT_EQ(H.info_bcast, 1u);
    send_info(3); /* the 5-minute heartbeat */
    send_info(3);
    CT_EQ(H.info_bcast, 1u);
    H.rf_attached = false; /* the hub sees the link come up (without having seen it go down: the cache survives) */
    run(100);
    send_info(3);
    CT_EQ(H.info_bcast, 2u);
    send_info(3);
    CT_EQ(H.info_bcast, 2u); /* no storm: one per link-up */
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(600);
    send_info(3);
    CT_EQ(H.info_bcast, 3u);
}

/* A changed stored set is relayed at once even though caps and format are the same (the hub's cache stays true, and
 * clients skip a push that would change nothing); an identical repeat is not. */
static void test_remote_info_broadcast_when_the_stored_set_changes(void) {
    world();
    send_info(3);
    CT_EQ(H.info_bcast, 1u);
    send_info_stored(3, 0x10, 4);
    CT_EQ(H.info_bcast, 2u);
    send_info_stored(3, 0x10, 4);
    CT_EQ(H.info_bcast, 2u);
    send_info_stored(3, 0x10, 3); /* same gen, fewer pages: the store was cleared and refilled */
    CT_EQ(H.info_bcast, 3u);
}

/* ---- theme_push: queued like a page_push, delivered before the pages of its gen ---- */
typedef struct { uint8_t type, key; char c; } seen_t; /* key: the page id of a page_push, the gen of a theme_push, the tile of a tile message (c: its text's first byte) */
static seen_t seen[64];
static int nseen;
static void th_hook(const hp_rf_msg_t *m) {
    hp_rf_val_t v[5];
    if (m->type == HP_MSG_TILE_UPDATE || m->type == HP_MSG_TILE_STATE) {
        size_t nv = m->type == HP_MSG_TILE_UPDATE ? 4 : 5;
        if (hp_rf_unpack(m->type, m->payload, m->len, v, nv) == 0 && nseen < 64)
            seen[nseen++] = (seen_t){(uint8_t)m->type, (uint8_t)v[2].u, (char)v[nv - 1].p[0]};
        return;
    }
    if ((m->type != HP_MSG_PAGE_PUSH && m->type != HP_MSG_THEME_PUSH) || hp_rf_unpack(m->type, m->payload, m->len, v, 3) != 0) return;
    if (nseen < 64) seen[nseen++] = (seen_t){(uint8_t)m->type, (uint8_t)v[1].u, 0};
    if (m->type == HP_MSG_PAGE_PUSH) (void)hp_store_put(&ST, v[2].p, v[2].len);
    else (void)hp_store_theme(&ST, (uint8_t)v[1].u, v[2].p, v[2].len);
}
static void th_reset(void) { nseen = 0; hp_store_init(&ST); }
static void th_world(void) {
    remote_hook = th_hook;
    world_reset_hook = th_reset;
    world_init(true, MODEL_S3);
    run(400);
    CT_CHECK(R.up);
}
static int push_theme(uint8_t gen, uint8_t seed) {
    uint8_t t[HP_THEME_BYTES];
    memset(t, seed, sizeof t);
    t[0] = 1;
    char b64[200], js[320];
    hp_base64_encode(t, sizeof t, b64);
    snprintf(js, sizeof js, "{\"remote\":1,\"gen\":%u,\"data\":\"%s\"}", gen, b64);
    cJSON *d = cJSON_Parse(js);
    char reply[512];
    size_t len = 0;
    int rc = TBL.handler[HP_MSG_THEME_PUSH](&H, NULL, 7, d, reply, sizeof reply, &len);
    cJSON_Delete(d);
    reply[len < sizeof reply ? len : sizeof reply - 1] = 0;
    return rc == 0 && strstr(reply, "\"ok\":true");
}
static size_t themed(uint8_t *o, uint8_t id, uint8_t total, uint8_t gen) {
    size_t n = mkpage(o, id, total, gen, 0, 0);
    o[7] = (uint8_t)(HP_PAGE_THEME_CUSTOM << 3);
    return n;
}
static void away(void) {
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
}

/* the theme is queued first and arrives first; the set is complete when its pages are in */
static void test_theme_arrives_before_its_pages(void) {
    th_world();
    uint8_t raw[40];
    CT_CHECK(push_theme(5, 1));
    for (uint8_t i = 0; i < 3; i++) CT_CHECK(push_raw(i, raw, themed(raw, i, 3, 5)));
    run(5000);
    CT_EQ(nseen, 4);
    CT_EQ(seen[0].type, HP_MSG_THEME_PUSH);
    for (int i = 1; i < 4; i++) CT_EQ(seen[i].type, HP_MSG_PAGE_PUSH);
    CT_CHECK(hp_store_complete(&ST) && hp_store_theme_ok(&ST));
    CT_EQ(H.npend, 0);
}

/* held while the remote is away: one theme per remote (the latest wins), a newer gen drops the old set and its theme,
 * and what is delivered when it returns is the new theme first */
static void test_held_themes_supersede_and_a_newer_gen_cancels_the_old_set(void) {
    th_world();
    away();
    uint8_t raw[40];
    CT_CHECK(push_theme(5, 1));
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 2, 5)));
    CT_CHECK(push_theme(5, 2)); /* same gen, new bytes: replaces the held one (same key) */
    CT_EQ(count_pend(HP_MSG_THEME_PUSH), 1);
    CT_CHECK(push_raw(1, raw, themed(raw, 1, 2, 5)));
    CT_EQ(H.npend, 3);
    CT_CHECK(push_theme(6, 3)); /* the config changed: the whole old set (pages and theme) goes */
    CT_EQ(H.npend, 1);
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 1, 6)));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 1);
    CT_EQ(count_pend(HP_MSG_THEME_PUSH), 1);
    CT_EQ(H.pend[0].type, HP_MSG_THEME_PUSH);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(8000);
    CT_EQ(nseen, 2);
    CT_EQ(seen[0].type, HP_MSG_THEME_PUSH);
    CT_EQ(seen[0].key, 6);
    CT_EQ(seen[1].type, HP_MSG_PAGE_PUSH);
    CT_CHECK(hp_store_complete(&ST) && ST.gen == 6);
    CT_EQ(H.npend, 0);
}

/* a lost theme is resent, bounded, and its pages do not overtake it */
static void test_failed_theme_is_resent_before_its_pages_and_bounded(void) {
    th_world();
    uint8_t raw[40];
    R.drop_dl = 1000; /* everything the hub transmits is lost */
    CT_CHECK(push_theme(5, 1));
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 1, 5)));
    run(4000);
    CT_EQ(nseen, 0);
    CT_CHECK(R.up);
    R.drop_dl = 0;
    run(8000);
    CT_EQ(nseen, 2);
    CT_EQ(seen[0].type, HP_MSG_THEME_PUSH); /* the theme first, though both failed */
    CT_EQ(seen[1].type, HP_MSG_PAGE_PUSH);
    CT_CHECK(hp_store_complete(&ST));
    CT_EQ(H.npend, 0);
    /* a remote that never answers: HUB_PAGE_RESENDS resends, then dropped and counted */
    uint32_t f0 = hp_rf_link_status(&H.link).failures, d0 = H.pend_dropped;
    R.drop_dl = 100000;
    CT_CHECK(push_theme(6, 4));
    run(30000);
    CT_EQ(hp_rf_link_status(&H.link).failures - f0, 1u + HUB_PAGE_RESENDS);
    CT_CHECK(H.pend_dropped > d0);
    CT_EQ(count_pend(HP_MSG_THEME_PUSH), 0);
}

/* a stale failed theme (a newer gen was pushed meanwhile) is not resent */
static void test_stale_failed_theme_is_not_resent(void) {
    th_world();
    R.drop_dl = 1000;
    CT_CHECK(push_theme(5, 1));
    run(500);
    R.drop_dl = 100000;
    CT_CHECK(push_theme(6, 2));
    run(30000);
    CT_EQ(H.npend, 0);
    R.drop_dl = 0;
    run(2000);
    for (int i = 0; i < nseen; i++) CT_CHECK(seen[i].key != 5 || seen[i].type != HP_MSG_THEME_PUSH);
}

static int first_seen(bool tile) {
    for (int i = 0; i < nseen; i++)
        if ((seen[i].type == HP_MSG_TILE_UPDATE || seen[i].type == HP_MSG_TILE_STATE) == tile && (tile || seen[i].type == HP_MSG_PAGE_PUSH)) return i;
    return -1;
}

/* a burst that fills the 8-slot link queue must not lose the theme or a page: they are held and sent when there is room */
static void test_busy_link_holds_theme_and_page(void) {
    th_world();
    uint8_t raw[40];
    for (uint8_t t = 0; t < HP_LINK_QUEUE; t++) CT_CHECK(tile_cmd(1, t, "x", -1));
    CT_CHECK(push_theme(5, 1));
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 1, 5)));
    run(12000);
    int th = -1, pg = -1;
    for (int i = 0; i < nseen; i++) {
        if (seen[i].type == HP_MSG_THEME_PUSH) th = i;
        if (seen[i].type == HP_MSG_PAGE_PUSH) pg = i;
    }
    CT_CHECK(th >= 0 && pg > th);
    CT_CHECK(hp_store_complete(&ST) && hp_store_theme_ok(&ST));
    CT_EQ(H.npend, 0);
    CT_EQ(H.theme_out, 0); /* every send was counted once and resolved once */
    CT_EQ(H.page_out[0], 0);
}

/* a link drop keeps the strict link's queue: the theme still queued is still counted, so its pages do not overtake it */
static void test_link_drop_keeps_counting_what_the_link_still_holds(void) {
    th_world();
    uint8_t raw[40];
    R.drop_dl = 1000; /* nothing gets through: the theme stays queued on the link */
    CT_CHECK(push_theme(5, 1));
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 1, 5)));
    CT_EQ(H.theme_out, 1);
    CT_CHECK(hp_rf_link_status(&H.link).queued > 0);
    away();
    CT_CHECK(hp_rf_link_status(&H.link).queued == 0 || H.theme_out == 1); /* zeroed only when the queue is empty */
    hp_rf_link_set_awake(&RL, true, R.now);
    R.drop_dl = 0;
    run(15000);
    int th = -1, pg = -1;
    for (int i = 0; i < nseen; i++) {
        if (seen[i].type == HP_MSG_THEME_PUSH && th < 0) th = i;
        if (seen[i].type == HP_MSG_PAGE_PUSH && pg < 0) pg = i;
    }
    CT_CHECK(th >= 0 && pg > th);
    CT_EQ(H.theme_out, 0);
}

/* live tile values during a themed push reach the remote after their page, once per tile, latest value */
static void test_tiles_follow_the_theme_and_their_page(void) {
    th_world();
    uint8_t raw[40];
    CT_CHECK(push_theme(5, 1));
    CT_CHECK(push_raw(0, raw, themed(raw, 0, 1, 5)));
    CT_CHECK(tile_cmd(0, 3, "A", -1));
    CT_CHECK(tile_cmd(0, 3, "B", -1));
    CT_CHECK(tile_cmd(0, 4, "C", -1));
    run(12000);
    int pg = first_seen(false), tl = first_seen(true);
    CT_CHECK(pg >= 0 && tl > pg);
    int n3 = 0, n4 = 0;
    for (int i = 0; i < nseen; i++) {
        if (seen[i].key == 3 && seen[i].type == HP_MSG_TILE_UPDATE) { n3++; CT_EQ(seen[i].c, 'B'); }
        if (seen[i].key == 4 && seen[i].type == HP_MSG_TILE_UPDATE) n4++;
    }
    CT_EQ(n3, 1);
    CT_EQ(n4, 1);
    CT_EQ(H.npend, 0);
}

/* the same without a theme: a page held for a failed delivery is not overtaken by tiles of that page */
static void test_tiles_do_not_overtake_a_resent_page(void) {
    th_world();
    uint8_t raw[40];
    R.drop_dl = 1000;
    CT_CHECK(push_raw(0, raw, mkpage(raw, 0, 1, 7, 0, 0)));
    for (int i = 0; i < 400 && !count_pend(HP_MSG_PAGE_PUSH); i++) run(50); /* until the failed page is held again */
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 1);
    CT_CHECK(tile_cmd(0, 3, "T", -1));
    R.drop_dl = 0;
    run(12000);
    int pg = first_seen(false), tl = first_seen(true);
    CT_CHECK(pg >= 0 && tl > pg);
    CT_EQ(H.npend, 0);
}

/* ---- Now Playing: now_playing / cover_chunk holds (stream H) ---- */
static bool hub_cmd(const char *js) {
    cJSON *d = cJSON_Parse(js);
    char reply[512];
    size_t len = 0;
    int rc = TBL.handler[HP_MSG_COMMAND](&H, NULL, 9, d, reply, sizeof reply, &len);
    cJSON_Delete(d);
    reply[len < sizeof reply ? len : sizeof reply - 1] = 0;
    return rc == 0 && strstr(reply, "\"ok\":true");
}
static uint8_t COVER[HUB_COVER_BYTES];
static bool cover_cmd(uint32_t cid, uint8_t fill) {
    static char b64[2600], js[3000];
    for (size_t i = 0; i < sizeof COVER; i++) COVER[i] = (uint8_t)(fill + i);
    hp_base64_encode(COVER, sizeof COVER, b64);
    snprintf(js, sizeof js, "{\"device\":\"hub\",\"command\":\"cover\",\"args\":{\"remote\":1,\"cid\":%u,\"data\":\"%s\"}}", cid, b64);
    return hub_cmd(js);
}
static bool np_cmd(uint32_t cid, int flags, int pos) {
    char js[300];
    snprintf(js, sizeof js,
             "{\"device\":\"hub\",\"command\":\"now_playing\",\"args\":{\"remote\":1,\"cid\":%u,\"flags\":%d,\"pos\":%d,\"dur\":3000,\"t1\":\"Show\",\"t2\":\"S1 E2\"}}",
             cid, flags, pos);
    return hub_cmd(js);
}
/* chunks of `cid` received by the remote; checks they tile the cover in order. Returns the count. */
static int chunks_of(uint32_t cid, uint8_t fill) {
    int n = 0;
    uint32_t want = 0;
    for (int i = 0; i < nlog; i++) {
        hp_rf_val_t v[4];
        if (rlog[i].type != HP_MSG_COVER_CHUNK || hp_rf_unpack(HP_MSG_COVER_CHUNK, rlog[i].payload, rlog[i].len, v, 4) != 0 || v[1].u != cid)
            continue;
        if (v[2].u == 0) want = 0; /* a fresh run of this cid */
        CT_EQ(v[2].u, want);
        for (size_t k = 0; k < v[3].len; k++) CT_EQ(v[3].p[k], (uint8_t)(fill + want + k));
        want += (uint32_t)v[3].len;
        n++;
    }
    return n;
}
static int count_type(uint8_t type) {
    int n = 0;
    for (int i = 0; i < nlog; i++) n += rlog[i].type == type;
    return n;
}

static void test_cover_is_chunked_and_sent_once_per_cid(void) {
    world();
    send_info(3);
    CT_CHECK(cover_cmd(11, 1));
    run(4000);
    CT_EQ(chunks_of(11, 1), 8); /* 7 x 220 + 220 = 1760 */
    CT_CHECK(cover_cmd(11, 1)); /* the same cid again: nothing more */
    CT_CHECK(cover_cmd(12, 2));
    run(4000);
    CT_EQ(count_type(HP_MSG_COVER_CHUNK), 16);
    CT_CHECK(cover_cmd(11, 1)); /* an older cid the remote still caches: not resent */
    run(600);
    CT_EQ(count_type(HP_MSG_COVER_CHUNK), 16);
    char bad[] = "{\"device\":\"hub\",\"command\":\"cover\",\"args\":{\"remote\":1,\"cid\":5,\"data\":\"AAAA\"}}";
    CT_CHECK(!hub_cmd(bad)); /* not 1760 bytes */
}

static void test_cover_hold_supersedes_never_evicts_pages_and_skips_sleep(void) {
    world();
    send_info(3);
    hp_rf_link_set_awake(&RL, false, R.now); /* asleep */
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    for (uint8_t i = 0; i < HUB_PEND_MAX; i++) { /* v1 pages: distinct ids, no gen purge */
        uint8_t b[] = {1, i, 1, 1, 0};
        CT_CHECK(push_raw(i, b, sizeof b));
    }
    (void)raw;
    CT_EQ(H.npend, HUB_PEND_MAX); /* the page queue is full */
    CT_CHECK(cover_cmd(21, 1));
    CT_CHECK(cover_cmd(22, 9)); /* supersedes the held 21 */
    CT_CHECK(np_cmd(22, 1, 100));
    CT_EQ(H.npend, HUB_PEND_MAX); /* no page lost to the cover or the now_playing */
    CT_EQ(H.pend_dropped, 0u);
    CT_EQ(count_type(HP_MSG_COVER_CHUNK) + count_type(HP_MSG_NOW_PLAYING), 0); /* nothing over the sleep path */
    run(2000);
    CT_EQ(count_type(HP_MSG_COVER_CHUNK), 0);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(150000); /* 255 held pages go first */
    CT_EQ(chunks_of(21, 1), 0); /* superseded: never sent */
    CT_EQ(chunks_of(22, 9), 8);
    const hp_rf_msg_t *m = last_of(HP_MSG_NOW_PLAYING);
    CT_CHECK(m != NULL);
    if (m) {
        hp_rf_val_t v[7];
        CT_EQ(hp_rf_unpack(HP_MSG_NOW_PLAYING, m->payload, m->len, v, 7), 0);
        CT_EQ(v[1].u, 22);
        CT_CHECK(v[3].u >= 102 && v[3].u <= 110); /* pos moved on while it was held (>= 2 s) */
        CT_EQ(v[4].u, 3000);
        CT_EQ(v[5].len, 4);
    }
}

static void test_cover_is_sent_again_after_a_rejoin(void) {
    world();
    send_info(3);
    CT_CHECK(cover_cmd(31, 3));
    run(4000);
    CT_EQ(chunks_of(31, 3), 8);
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(8000);
    CT_CHECK(chunks_of(31, 3) >= 16);
    CT_CHECK(count_type(HP_MSG_NOW_PLAYING) == 0);
}

int main(void) {
    test_link_drop_keeps_counting_what_the_link_still_holds();
    test_busy_link_holds_theme_and_page();
    test_tiles_follow_the_theme_and_their_page();
    test_tiles_do_not_overtake_a_resent_page();
    test_theme_arrives_before_its_pages();
    test_held_themes_supersede_and_a_newer_gen_cancels_the_old_set();
    test_failed_theme_is_resent_before_its_pages_and_bounded();
    test_stale_failed_theme_is_not_resent();
    test_remote_info_broadcast_when_the_stored_set_changes();
    test_remote_info_cached_and_cleared_on_link_loss();
    test_unknown_and_unexpected_types_are_dropped();
    test_tile_state_only_for_remotes_that_announce_it();
    test_tile_update_and_state_share_one_held_key();
    test_newer_gen_drops_held_pages_of_older_gen();
    test_max_held_pages_fill_the_store_in_order();
    test_tile_burst_never_evicts_pages();
    test_full_queue_of_pages_refuses_tiles_and_drops_oldest_page();
    test_failed_page_push_is_resent_while_attached();
    test_page_resends_are_bounded();
    test_remote_info_broadcast_once_per_link_up();
    test_cover_is_chunked_and_sent_once_per_cid();
    test_cover_hold_supersedes_never_evicts_pages_and_skips_sleep();
    test_cover_is_sent_again_after_a_rejoin();
    return CT_DONE();
}
