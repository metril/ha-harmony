/* Page format v2 over the fake radio (tests/c/fake_radio.h): remote_info caching, tile_state vs tile_update
 * selection and the shared held-message key, held pages of an older gen, and the page store filling from 40 pages
 * held while the remote was away. Real hub context and real hp_rf_link_t ends. */
#include "fake_radio.h"
#include "hub/net/ws.h"
#include "remote/page/page.h"
#include "remote/page/store.h"

#define LOG_MAX 128
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

static void send_info(uint32_t caps) {
    hp_rf_val_t v[4] = {{.u = 1}, {.u = 2}, {.u = caps}, {.u = 50}};
    uint8_t pl[HP_RF_MSG_MAX];
    size_t n;
    CT_EQ(hp_rf_pack(HP_MSG_REMOTE_INFO, v, 4, pl, sizeof pl, &n), 0);
    CT_EQ(hp_rf_link_send(&RL, HP_MSG_REMOTE_INFO, pl, n, R.now), 0);
    run(300);
}

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
    }
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

/* 40 pages (32 of a config, 8 more of a bigger one would not exist: the cap is the store's 32) held while the
 * remote is away are all delivered, in order, when it comes back, and the page store fills from them. */
static hp_store_t ST;
static uint8_t order[64];
static int norder;
static void store_hook(const hp_rf_msg_t *m) {
    hp_rf_val_t v[3];
    if (m->type != HP_MSG_PAGE_PUSH || hp_rf_unpack(m->type, m->payload, m->len, v, 3) != 0) return;
    if (norder < 64) order[norder++] = (uint8_t)v[1].u;
    CT_EQ(hp_store_put(&ST, v[2].p, v[2].len), HP_STORE_OK);
}
static void store_reset(void) { norder = 0; hp_store_init(&ST); }

static void test_forty_held_pages_fill_the_store_in_order(void) {
    remote_hook = store_hook;
    world_reset_hook = store_reset;
    world_init(true, MODEL_S3);
    run(400);
    hp_rf_link_set_awake(&RL, false, R.now);
    run(900);
    CT_CHECK(!R.up);
    uint8_t raw[40];
    for (uint8_t id = 0; id < 32; id++) CT_CHECK(push_raw(id, raw, mkpage(raw, id, 32, 0x55, id == 3 ? 1 : 0, 0)));
    for (uint8_t t = 0; t < 8; t++) CT_CHECK(tile_cmd((uint8_t)(t + 1), 1, "t", -1));
    CT_EQ(H.npend, 40); /* HUB_PEND_MAX: nothing was pushed out */
    run(300);
    CT_EQ(norder, 0);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(20000);
    CT_EQ(norder, 32);
    for (int i = 0; i < norder; i++) CT_EQ(order[i], i); /* in order */
    CT_EQ(H.npend, 0);
    CT_EQ(hp_store_count(&ST), 32);
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
    for (int t = 0; t < 100; t++) CT_CHECK(tile_cmd((uint8_t)(t / 10), (uint8_t)(t % 10), "v", -1));
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), 10);
    CT_EQ(H.npend, HUB_PEND_MAX);
    /* the newest tiles are the ones kept */
    CT_CHECK(H.pend[H.npend - 1].type == HP_MSG_TILE_UPDATE && H.pend[H.npend - 1].page == 9 && H.pend[H.npend - 1].tile == 9);
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
    for (uint8_t id = 0; id < HUB_PEND_MAX; id++) {
        uint8_t b[sizeof v1];
        memcpy(b, v1, sizeof v1);
        b[1] = id;
        CT_CHECK(push_raw(id, b, sizeof b));
    }
    CT_EQ(H.npend, HUB_PEND_MAX);
    CT_CHECK(tile_cmd(1, 1, "x", -1)); /* dropped: still ok to the caller, no page lost */
    CT_EQ(count_pend(HP_MSG_PAGE_PUSH), HUB_PEND_MAX);
    CT_EQ(H.pend_dropped, 1u);
    CT_CHECK(push_raw(50, raw, mkpage(raw, 0, 1, 0x70, 0, 0)));
    CT_EQ(H.npend, HUB_PEND_MAX);
    CT_EQ(H.pend[0].page, 1); /* page 0 was the oldest */
    CT_EQ(H.pend_dropped, 2u);
}

int main(void) {
    test_remote_info_cached_and_cleared_on_link_loss();
    test_unknown_and_unexpected_types_are_dropped();
    test_tile_state_only_for_remotes_that_announce_it();
    test_tile_update_and_state_share_one_held_key();
    test_newer_gen_drops_held_pages_of_older_gen();
    test_forty_held_pages_fill_the_store_in_order();
    test_tile_burst_never_evicts_pages();
    test_full_queue_of_pages_refuses_tiles_and_drops_oldest_page();
    return CT_DONE();
}
