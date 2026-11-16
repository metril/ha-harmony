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
    CT_EQ(hp_sim_count(&sim), 2);
    /* release/hold do not re-fire the action but are still forwarded */
    hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_RELEASE, on_emit, &e);
    hp_key_handle(&m, &sinks, 1, 2, HP_KEY_ACTION_HOLD, on_emit, &e);
    CT_EQ(e.n, 3);
    CT_EQ(hp_sim_count(&sim), 2);
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
    CT_RUN(test_sim_rf_single_owner);
    return CT_DONE();
}
