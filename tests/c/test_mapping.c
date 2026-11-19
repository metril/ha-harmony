#define _XOPEN_SOURCE 700 /* mkstemp: uClibc gates it behind XOPEN, not POSIX_C_SOURCE */
#include <stdlib.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/engine/mapping.h"
#include "hub/hp_errors.h"
#include "hub/io/sim_sinks.h"

static const char *CFG =
    "{\"keys\":{\"1\":{\"type\":\"ha\",\"event\":\"menu\"},\"2\":{\"type\":\"bt\",\"usage\":233}},"
    "\"activities\":["
    "{\"name\":\"TV\",\"keys\":{\"2\":{\"type\":\"ir\",\"port_mask\":3,\"pronto\":[0,109,1,0,24,24]},"
    "\"5\":{\"type\":\"bt\",\"usage\":234}},"
    "\"steps\":[{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,109,1,0,10,20]},"
    "{\"type\":\"bt\",\"usage\":48},{\"type\":\"ir\",\"port_mask\":4,\"pronto\":[0,109,1,0,30,40]}],"
    "\"off\":[{\"type\":\"ir\",\"port_mask\":255,\"pronto\":[0,109,1,0,50,60]}]},"
    "{\"name\":\"Music\",\"steps\":[{\"type\":\"none\"}]}]}";

static hp_sim_t sim;
static hp_sinks_t sinks;

static void setup(void) {
    hp_sim_init(&sim, NULL);
    sinks = hp_sim_sinks(&sim);
}

static void test_load_lookup(void) {
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    CT_EQ(hp_map_activity_count(&m), 2);
    CT_CHECK(!strcmp(hp_map_activity_name(&m, 1), "Music"));
    hp_action_t a;
    CT_EQ(hp_map_lookup(&m, "TV", 2, &a), 1); /* activity overrides global */
    CT_EQ(a.type, HP_ACT_IR);
    CT_EQ(a.port_mask, 3);
    CT_EQ(hp_map_lookup(&m, "TV", 1, &a), 1); /* falls back to global */
    CT_EQ(a.type, HP_ACT_HA_EVENT);
    CT_CHECK(!strcmp(a.event, "menu"));
    CT_EQ(hp_map_lookup(&m, "Music", 2, &a), 1);
    CT_EQ(a.type, HP_ACT_BT);
    CT_EQ(hp_map_lookup(&m, NULL, 2, &a), 1); /* global only */
    CT_EQ(hp_map_lookup(&m, "TV", 99, &a), 0); /* unmapped */
    CT_EQ(a.type, HP_ACT_NONE);
    CT_EQ(hp_map_lookup(&m, "Nope", 1, &a), -HP_E_INVAL);
    hp_map_free(&m);
}

static void test_apply(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    hp_action_t a;
    hp_map_lookup(&m, "TV", 2, &a);
    CT_EQ(hp_map_apply(&m, &a, &sinks), 0);
    CT_EQ(hp_sim_count(&sim), 1);
    const hp_sim_rec_t *r = hp_sim_rec(&sim, 0);
    CT_EQ(r->kind, HP_SIM_IR);
    CT_EQ(r->port_mask, 3);
    CT_EQ(r->n, 2);
    CT_EQ(r->hz, 38029);
    CT_EQ(r->us[0], 631);
    CT_EQ(r->us[1], 631);
    hp_map_lookup(&m, "Music", 2, &a);
    CT_EQ(hp_map_apply(&m, &a, &sinks), 0);
    CT_EQ(hp_sim_count(&sim), 3); /* BT tap = down + up */
    CT_EQ(hp_sim_rec(&sim, 1)->kind, HP_SIM_BT);
    CT_EQ(hp_sim_rec(&sim, 1)->usage, 233);
    CT_EQ(hp_sim_rec(&sim, 1)->down, 1);
    CT_EQ(hp_sim_rec(&sim, 2)->down, 0);
    hp_map_lookup(&m, "TV", 99, &a); /* NONE */
    CT_EQ(hp_map_apply(&m, &a, &sinks), 0);
    CT_EQ(hp_sim_count(&sim), 3);
    hp_map_free(&m);
}

static void test_activity(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    hp_map_load(&m, CFG, strlen(CFG));
    CT_EQ(hp_activity_set(&m, "TV", &sinks), 0);
    CT_EQ(hp_sim_count(&sim), 4); /* ir, bt down, bt up, ir */
    CT_EQ(hp_sim_rec(&sim, 0)->port_mask, 1);
    CT_EQ(hp_sim_rec(&sim, 1)->kind, HP_SIM_BT);
    CT_EQ(hp_sim_rec(&sim, 3)->port_mask, 4);
    CT_EQ(hp_map_current(&m), 0);
    CT_EQ(hp_activity_set(&m, "Nope", &sinks), -HP_E_INVAL);
    CT_EQ(hp_map_current(&m), 0);
    CT_EQ(hp_activity_off(&m, &sinks), 0); /* returns previous index */
    CT_EQ(hp_sim_count(&sim), 5);
    CT_EQ(hp_sim_rec(&sim, 4)->port_mask, 255);
    CT_EQ(hp_map_current(&m), -1);
    CT_EQ(hp_activity_off(&m, &sinks), -HP_E_INVAL);
    hp_map_free(&m);
}

static void test_bad_config_keeps_old(void) {
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    const char *bad[] = {
        "{not json",
        "[]",
        "{\"activities\":[{\"name\":\"A\"},{\"name\":\"A\"}]}",
        "{\"activities\":[{\"name\":\"\"}]}",
        "{\"keys\":{\"x\":{\"type\":\"none\"}}}",
        "{\"keys\":{\"70000\":{\"type\":\"none\"}}}",
        "{\"keys\":{\"1\":{\"type\":\"warp\"}}}",
        "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":0,\"pronto\":[0,0,0,0]}}}",
        "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,0,70000,0]}}}",
        "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,0]}}}",
        "{\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":-1}}}",
        "{\"keys\":{\"1\":{\"type\":\"none\"},\"1\":{\"type\":\"none\"}}}",
        "{\"activities\":[{\"name\":\"A\",\"steps\":[{\"type\":\"none\"}],\"keys\":{\"1\":5}}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int r = hp_map_load(&m, bad[i], strlen(bad[i]));
        if (r >= 0) fprintf(stderr, "accepted: %s\n", bad[i]);
        CT_CHECK(r < 0);
        CT_EQ(hp_map_activity_count(&m), 2); /* old map intact */
        hp_action_t a;
        CT_EQ(hp_map_lookup(&m, "TV", 5, &a), 1);
    }
    hp_map_free(&m);
}

static char big[200000];

static void test_limits(void) {
    hp_map_t m;
    hp_map_init(&m);
    size_t n = (size_t)snprintf(big, sizeof big, "{\"keys\":{");
    for (int i = 0; i <= HP_MAP_MAX_KEYS; i++)
        n += (size_t)snprintf(big + n, sizeof big - n, "%s\"%d\":{\"type\":\"none\"}", i ? "," : "", i);
    n += (size_t)snprintf(big + n, sizeof big - n, "}}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    n = (size_t)snprintf(big, sizeof big, "{\"activities\":[");
    for (int i = 0; i <= HP_MAP_MAX_ACTIVITIES; i++)
        n += (size_t)snprintf(big + n, sizeof big - n, "%s{\"name\":\"a%d\"}", i ? "," : "", i);
    n += (size_t)snprintf(big + n, sizeof big - n, "]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    n = (size_t)snprintf(big, sizeof big, "{\"activities\":[{\"name\":\"a\",\"steps\":[");
    for (int i = 0; i <= HP_MAP_MAX_STEPS; i++)
        n += (size_t)snprintf(big + n, sizeof big - n, "%s{\"type\":\"none\"}", i ? "," : "");
    n += (size_t)snprintf(big + n, sizeof big - n, "]}]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    n = (size_t)snprintf(big, sizeof big, "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,0,0,0");
    for (int i = 0; i < HP_MAP_MAX_PRONTO; i++) n += (size_t)snprintf(big + n, sizeof big - n, ",0");
    n += (size_t)snprintf(big + n, sizeof big - n, "]}}}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    hp_map_free(&m);
}

static void test_cache(void) {
    char path[] = "/tmp/hp_cache_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    hp_map_t m, m2;
    hp_map_init(&m);
    hp_map_init(&m2);
    CT_EQ(hp_map_save_cache(&m, path), -HP_E_INVAL); /* nothing loaded */
    hp_map_load(&m, CFG, strlen(CFG));
    CT_EQ(hp_map_save_cache(&m, path), 0);
    CT_EQ(hp_map_load_cache(&m2, path), 0);
    CT_EQ(hp_map_activity_count(&m2), 2);
    hp_action_t a;
    CT_EQ(hp_map_lookup(&m2, "TV", 5, &a), 1);

    /* corrupt / truncated files are rejected and leave the map alone */
    FILE *f = fopen(path, "rb");
    static unsigned char raw[70000];
    size_t len = fread(raw, 1, sizeof raw, f);
    fclose(f);
    CT_CHECK(len > 20);
    raw[len / 2] ^= 0x55;
    f = fopen(path, "wb");
    fwrite(raw, 1, len, f);
    fclose(f);
    CT_EQ(hp_map_load_cache(&m2, path), -HP_E_INVAL);
    raw[len / 2] ^= 0x55;
    f = fopen(path, "wb");
    fwrite(raw, 1, len - 3, f); /* truncated */
    fclose(f);
    CT_EQ(hp_map_load_cache(&m2, path), -HP_E_INVAL);
    f = fopen(path, "wb");
    fwrite("HPM", 1, 3, f);
    fclose(f);
    CT_EQ(hp_map_load_cache(&m2, path), -HP_E_INVAL);
    CT_EQ(hp_map_activity_count(&m2), 2);
    unlink(path);
    CT_EQ(hp_map_load_cache(&m2, path), -HP_E_NOENT);
    CT_EQ(hp_map_activity_count(&m2), 2);
    hp_map_free(&m);
    hp_map_free(&m2);
}

typedef struct { int n; uint8_t remote, key, action; } emit_t;
static void on_emit(void *ctx, uint8_t remote, uint8_t key, hp_key_action_t action) {
    emit_t *e = ctx;
    e->n++; e->remote = remote; e->key = key; e->action = (uint8_t)action;
}

static void test_key_handle(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    hp_map_load(&m, CFG, strlen(CFG));
    emit_t e = {0};
    /* no activity: global map; BT key 2 */
    CT_EQ(hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_PRESS, on_emit, &e), 0);
    CT_EQ(e.n, 1);
    CT_EQ(hp_sim_count(&sim), 1); /* a physical press is key down only ... */
    CT_EQ(hp_sim_rec(&sim, 0)->down, 1);
    hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_HOLD, on_emit, &e); /* hold sends nothing */
    CT_EQ(hp_sim_count(&sim), 1);
    hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_RELEASE, on_emit, &e); /* ... key up at the release */
    CT_EQ(e.n, 3);
    CT_EQ(hp_sim_count(&sim), 2);
    CT_EQ(hp_sim_rec(&sim, 1)->down, 0);
    /* unmapped key still emits */
    CT_EQ(hp_key_handle(&m, &sinks, 3, 200, HP_KEY_ACTION_PRESS, on_emit, &e), 0);
    CT_EQ(e.n, 4);
    CT_EQ(e.remote, 3);
    CT_EQ(e.key, 200);
    CT_EQ(hp_sim_count(&sim), 2);
    /* with TV active, key 2 is IR */
    hp_activity_set(&m, "TV", &sinks);
    size_t c = hp_sim_count(&sim);
    hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_PRESS, on_emit, &e);
    CT_EQ(hp_sim_count(&sim), c + 1);
    CT_EQ(hp_sim_rec(&sim, c)->kind, HP_SIM_IR);
    hp_map_free(&m);
}

static const char *BTCFG =
    "{\"format\":2,"
    "\"devices\":[{\"id\":\"tv\",\"commands\":{\"x\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":\"0000 006d 0001 0000 0018 0018\"}}},"
    "{\"id\":\"shield\",\"commands\":{\"Home\":{\"type\":\"bt\",\"page\":12,\"usage\":547,\"host\":0},"
    "\"Up\":{\"type\":\"bt\",\"page\":7,\"usage\":82,\"host\":0}}},"
    "{\"id\":\"box\",\"commands\":{\"Vol\":{\"type\":\"bt\",\"usage\":233,\"host\":15}}}],"
    "\"bt_hosts\":[{\"slot\":0,\"addr\":\"aa:bb:cc:dd:ee:ff\",\"name\":\"Shield\",\"transport\":\"le\"},"
    "{\"slot\":15,\"addr\":null,\"name\":\"Box\",\"transport\":\"auto\"}],"
    "\"activities\":[{\"id\":\"w\",\"devices\":[{\"device\":\"shield\"}],"
    "\"steps\":[{\"type\":\"device\",\"device\":\"shield\",\"command\":\"Home\"}],"
    "\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"shield\",\"command\":\"Up\"},"
    "\"2\":{\"type\":\"device\",\"device\":\"box\",\"command\":\"Vol\"}}}]}";

static void test_bt_hosts(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, BTCFG, strlen(BTCFG)), 0);
    CT_EQ(m.nbt_hosts, 2);
    const hp_bt_host_t *h0 = hp_map_bt_host(&m, 0), *h15 = hp_map_bt_host(&m, 15);
    CT_CHECK(h0 && h0->has_addr);
    CT_CHECK(!strcmp(h0->addr, "AA:BB:CC:DD:EE:FF")); /* upper-cased */
    CT_CHECK(!strcmp(h0->name, "Shield"));
    CT_EQ(h0->transport, HP_BT_LE);
    CT_CHECK(h15 && !h15->has_addr);
    CT_EQ(h15->transport, HP_BT_AUTO);
    CT_CHECK(!hp_map_bt_host(&m, 1));
    hp_action_t a;
    CT_EQ(hp_map_lookup(&m, "w", 1, &a), 1);
    CT_EQ(a.type, HP_ACT_BT);
    CT_EQ(a.page, 7);
    CT_EQ(a.usage, 82);
    CT_EQ(a.host, 0);
    CT_EQ(hp_map_lookup(&m, "w", 2, &a), 1);
    CT_EQ(a.page, 12); /* page absent = consumer */
    CT_EQ(a.host, 15);
    /* a step is a tap: down + up, with host and page */
    CT_EQ(hp_activity_set(&m, "w", &sinks), 0);
    CT_EQ(hp_sim_count(&sim), 2);
    CT_EQ(hp_sim_rec(&sim, 0)->page, 12);
    CT_EQ(hp_sim_rec(&sim, 0)->usage, 547);
    CT_EQ(hp_sim_rec(&sim, 0)->down, 1);
    CT_EQ(hp_sim_rec(&sim, 1)->down, 0);
    /* a key: down at the press, up at the release; a second press releases the first first */
    CT_EQ(hp_key_handle(&m, &sinks, 1, 1, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(hp_sim_count(&sim), 3);
    CT_EQ(hp_sim_rec(&sim, 2)->page, 7);
    CT_EQ(hp_sim_rec(&sim, 2)->down, 1);
    CT_EQ(hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(hp_sim_count(&sim), 5); /* up of key 1, down of key 2 */
    CT_EQ(hp_sim_rec(&sim, 3)->down, 0);
    CT_EQ(hp_sim_rec(&sim, 3)->usage, 82);
    CT_EQ(hp_sim_rec(&sim, 4)->host, 15);
    CT_EQ(hp_key_handle(&m, &sinks, 1, 1, HP_KEY_ACTION_RELEASE, NULL, NULL), 0); /* not the held key */
    CT_EQ(hp_sim_count(&sim), 5);
    CT_EQ(hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_RELEASE, NULL, NULL), 0);
    CT_EQ(hp_sim_count(&sim), 6);
    CT_EQ(hp_sim_rec(&sim, 5)->down, 0);
    CT_EQ(hp_sim_rec(&sim, 5)->host, 15);
    hp_map_free(&m);
}

static void test_bt_bad(void) {
    static const char *const bad[] = {
        "{\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":1,\"page\":9}}}",
        "{\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":1,\"host\":255}}}",
        "{\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":1,\"host\":-1}}}",
        "{\"bt_hosts\":[{\"slot\":255}]}",
        "{\"bt_hosts\":[{\"slot\":0},{\"slot\":0}]}",
        "{\"bt_hosts\":[{\"slot\":0,\"addr\":\"aa:bb\"}]}",
        "{\"bt_hosts\":[{\"slot\":0,\"transport\":\"wifi\"}]}",
        "{\"bt_hosts\":{}}",
    };
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, BTCFG, strlen(BTCFG)), 0);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int r = hp_map_load(&m, bad[i], strlen(bad[i]));
        if (r >= 0) fprintf(stderr, "accepted: %s\n", bad[i]);
        CT_CHECK(r < 0);
        CT_CHECK(hp_map_bt_host(&m, 0)); /* the old map is intact */
    }
    hp_map_free(&m);
}

typedef struct { int n; uint8_t remote[8], key[8], action[8]; } seq_t;
static void on_seq(void *ctx, uint8_t remote, uint8_t key, hp_key_action_t action) {
    seq_t *q = ctx;
    if (q->n < 8) { q->remote[q->n] = remote; q->key[q->n] = key; q->action[q->n] = (uint8_t)action; }
    q->n++;
}

static const char *BTRCFG =
    "{\"format\":2,"
    "\"keys\":{\"1\":{\"type\":\"bt\",\"usage\":233},\"8\":{\"type\":\"none\"}},"
    "\"bt_remotes\":[{\"slot\":0,\"addr\":\"11:22:33:44:55:66\",\"name\":\"Remote\","
    "\"keys\":[[7,82,1],[12,233,8],[12,547,34]]},{\"slot\":2,\"addr\":null,\"name\":\"B\",\"keys\":[]}]}";

static void test_bt_remote(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, BTRCFG, strlen(BTRCFG)), 0);
    const hp_bt_remote_t *r0 = hp_map_bt_remote(&m, 0), *r2 = hp_map_bt_remote(&m, 2);
    CT_CHECK(r0 && r0->has_addr && r0->nkeys == 3);
    CT_CHECK(!hp_map_bt_remote(&m, 1) && r2 && !r2->has_addr);
    seq_t q = {0};
    /* up arrow = key 1 = a bt action: press sends key down, hold nothing, release key up */
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 7, 82, true, on_seq, &q), 0);
    CT_EQ(q.n, 1);
    CT_EQ(q.remote[0], HH_BT_REMOTE_ID_BASE);
    CT_EQ(q.key[0], 1);
    CT_EQ(q.action[0], HP_KEY_ACTION_PRESS);
    CT_EQ(hp_sim_count(&sim), 1);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 7, 82, true, on_seq, &q), 0); /* auto-repeat */
    CT_EQ(q.action[1], HP_KEY_ACTION_HOLD);
    CT_EQ(hp_sim_count(&sim), 1);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 7, 82, false, on_seq, &q), 0);
    CT_EQ(q.action[2], HP_KEY_ACTION_RELEASE);
    CT_EQ(q.key[2], 1);
    CT_EQ(hp_sim_count(&sim), 2);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 7, 82, false, on_seq, &q), 0); /* up of nothing */
    CT_EQ(q.n, 3);
    /* unmapped usage, wrong page, unconfigured and out-of-range slots: ignored */
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 7, 99, true, on_seq, &q), 0);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 0, 12, 82, true, on_seq, &q), 0);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 1, 7, 82, true, on_seq, &q), 0);
    CT_EQ(hp_map_bt_remote_key(&m, &sinks, 9, 7, 82, true, on_seq, &q), 0);
    CT_EQ(q.n, 3);
    /* a second usage going down releases the first */
    hp_map_bt_remote_key(&m, &sinks, 0, 7, 82, true, on_seq, &q);
    hp_map_bt_remote_key(&m, &sinks, 0, 12, 547, true, on_seq, &q);
    CT_EQ(q.n, 6);
    CT_EQ(q.action[4], HP_KEY_ACTION_RELEASE);
    CT_EQ(q.key[4], 1);
    CT_EQ(q.action[5], HP_KEY_ACTION_PRESS);
    CT_EQ(q.key[5], 34);
    /* a reload while a key is down keeps its release */
    CT_EQ(hp_map_load(&m, BTRCFG, strlen(BTRCFG)), 0);
    hp_map_bt_remote_key(&m, &sinks, 0, 12, 547, false, on_seq, &q);
    CT_EQ(q.n, 7);
    CT_EQ(q.key[6], 34);
    hp_map_free(&m);
    CT_CHECK(!hp_map_bt_remote(&m, 0));
}

static void test_bt_remote_budget_and_bad(void) {
    static const char *const bad[] = {
        "{\"bt_remotes\":[{\"slot\":32}]}",
        "{\"bt_remotes\":[{\"slot\":0},{\"slot\":0}]}",
        "{\"bt_remotes\":[{\"slot\":0,\"keys\":[[9,1,1]]}]}",
        "{\"bt_remotes\":[{\"slot\":0,\"keys\":[[7,1,256]]}]}",
        "{\"bt_remotes\":[{\"slot\":0,\"keys\":[[7,1,1],[7,1,2]]}]}",
        "{\"bt_remotes\":[{\"slot\":0,\"keys\":[[7,1]]}]}",
        "{\"bt_remotes\":[{\"slot\":0,\"addr\":\"zz\"}]}",
        "{\"bt_remotes\":{}}",
    };
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, BTRCFG, strlen(BTRCFG)), 0);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int r = hp_map_load(&m, bad[i], strlen(bad[i]));
        if (r >= 0) fprintf(stderr, "accepted: %s\n", bad[i]);
        CT_CHECK(r < 0);
        const hp_bt_remote_t *rr = hp_map_bt_remote(&m, 0);
        CT_CHECK(rr && rr->nkeys == 3); /* the old map is intact */
    }
    hp_map_free(&m);
    /* the remote's keys count as key-map entries in the byte estimate */
    static const char *const none = "{\"format\":2,\"keys\":{}}";
    static const char *const three =
        "{\"format\":2,\"keys\":{},\"bt_remotes\":[{\"slot\":0,\"keys\":[[7,1,1],[7,2,2],[7,3,3]]}]}";
    size_t a = 0, b = 0;
    CT_EQ(hp_map_estimate(none, strlen(none), &a), 0);
    CT_EQ(hp_map_estimate(three, strlen(three), &b), 0);
    CT_EQ(b - a, 3 * HP_MAP_KEY_BYTES + HP_MAP_DEV_BYTES); /* the keys as key-map entries, the remote as a device */
}

/* The tables are allocated per load: the largest counts load, one more is refused. */
static char *many_tables(int hosts, int remotes) {
    size_t cap = 64 + (size_t)(hosts + remotes) * 96;
    char *t = malloc(cap), *w = t;
    w += sprintf(w, "{\"format\":2,\"bt_hosts\":[");
    for (int i = 0; i < hosts; i++) w += sprintf(w, "%s{\"slot\":%d,\"addr\":null}", i ? "," : "", i);
    w += sprintf(w, "],\"bt_remotes\":[");
    for (int i = 0; i < remotes; i++) w += sprintf(w, "%s{\"slot\":%d,\"keys\":[[7,%d,1]]}", i ? "," : "", i, i + 1);
    strcpy(w, "]}");
    return t;
}

static void test_bt_table_limits(void) {
    hp_map_t m;
    hp_map_init(&m);
    char *ok = many_tables(HH_BT_MAX_HOSTS, HH_BT_MAX_REMOTES);
    CT_EQ(hp_map_load(&m, ok, strlen(ok)), 0);
    CT_EQ(m.nbt_hosts, HH_BT_MAX_HOSTS);
    CT_EQ(m.nbt_remotes, HH_BT_MAX_REMOTES);
    CT_CHECK(hp_map_bt_host(&m, HH_BT_MAX_HOSTS - 1) && hp_map_bt_remote(&m, HH_BT_MAX_REMOTES - 1));
    char *h = many_tables(HH_BT_MAX_HOSTS + 1, 0);
    CT_EQ(hp_map_load(&m, h, strlen(h)), -HP_E_TOOBIG);
    char *r = many_tables(0, HH_BT_MAX_REMOTES + 1);
    CT_EQ(hp_map_load(&m, r, strlen(r)), -HP_E_TOOBIG);
    CT_EQ(m.nbt_hosts, HH_BT_MAX_HOSTS); /* a refused load leaves the old map */
    /* a tiny budget counts the tables: the same config is over it */
    hp_map_set_budget(&m, HP_MAP_SLACK_BYTES + 100 * HP_MAP_DEV_BYTES);
    CT_CHECK(hp_map_load(&m, ok, strlen(ok)) < 0);
    hp_map_free(&m);
    free(ok);
    free(h);
    free(r);
}

static void test_sim_rf_single_owner(void) {
    setup();
    hp_rf_transport *rf = sinks.rf;
    CT_EQ(rf->claim(rf->self), 0);
    CT_EQ(rf->claim(rf->self), -HP_E_BUSY);
    uint8_t fr[3] = {1, 2, 0}, out[8];
    CT_EQ(hp_sim_rf_script(&sim, fr, 3), 0);
    CT_EQ(rf->recv(rf->self, out, sizeof out, 0), 3);
    CT_EQ(out[1], 2);
    CT_EQ(rf->recv(rf->self, out, sizeof out, 0), 0);
}

int main(void) {
    CT_RUN(test_load_lookup);
    CT_RUN(test_apply);
    CT_RUN(test_activity);
    CT_RUN(test_bad_config_keeps_old);
    CT_RUN(test_limits);
    CT_RUN(test_cache);
    CT_RUN(test_key_handle);
    CT_RUN(test_bt_hosts);
    CT_RUN(test_bt_bad);
    CT_RUN(test_bt_remote);
    CT_RUN(test_bt_remote_budget_and_bad);
    CT_RUN(test_bt_table_limits);
    CT_RUN(test_sim_rf_single_owner);
    return CT_DONE();
}
