#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/engine/mapping.h"
#include "hub/hp_errors.h"
#include "hub/io/sim_sinks.h"

static const char *CFG =
    "{\"format\":2,"
    "\"devices\":[{\"id\":\"tv\",\"name\":\"TV\",\"settle_ms\":1500,"
    "  \"power\":{\"on\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":2},\"off\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":3}},"
    "  \"inputs\":{\"hdmi1\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":9}},"
    "  \"commands\":{\"vol_up\":{\"proto\":\"nec\",\"addr\":4,\"cmd\":16,\"port_mask\":3},"
    "               \"raw1\":{\"raw\":{\"hz\":38000,\"us\":[9000,4500,560,560]}},"
    "               \"pr\":{\"pronto\":\"0000 006d 0001 0000 0018 0018\"}}},"
    " {\"id\":\"amp\",\"power\":{\"toggle\":{\"proto\":\"rc5\",\"addr\":16,\"cmd\":12}},"
    "  \"commands\":{\"mute\":{\"proto\":\"rc5\",\"addr\":16,\"cmd\":13}}}],"
    "\"activities\":["
    "{\"id\":\"watch\",\"name\":\"Watch TV\",\"devices\":[{\"device\":\"tv\",\"input\":\"hdmi1\"},{\"device\":\"amp\"}],"
    " \"steps\":[{\"type\":\"device\",\"device\":\"tv\",\"command\":\"vol_up\"}],"
    " \"off\":[{\"type\":\"device\",\"device\":\"tv\",\"command\":\"power_off\"}],"
    " \"keys\":{\"2\":{\"type\":\"device\",\"device\":\"amp\",\"command\":\"mute\"}}},"
    "{\"id\":\"music\",\"steps\":[{\"type\":\"none\"}]}],"
    "\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"watch\"},"
    "\"3\":{\"type\":\"activity\",\"activity\":\"off\"},"
    "\"2\":{\"type\":\"ha\",\"event\":\"x\"},"
    "\"4\":{\"type\":\"device\",\"device\":\"tv\",\"input\":\"hdmi1\"},"
    "\"255\":{\"type\":\"ir\",\"proto\":\"samsung32\",\"addr\":7,\"cmd\":2}}}";

static hp_sim_t sim;
static hp_sinks_t sinks;
static hp_sched_t sched;
static hp_irout_t out;
static uint32_t clk(void *c) { (void)c; return 0; }

static void setup(void) {
    hp_sim_init(&sim, NULL);
    sinks = hp_sim_sinks(&sim);
    hp_sched_init(&sched, clk, NULL);
    hp_irout_init(&out, &sched);
}

static void test_load_and_resolve(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    hp_map_set_out(&m, &out);
    int r = hp_map_load(&m, CFG, strlen(CFG));
    if (r) fprintf(stderr, "err: %s\n", hp_map_last_error(&m));
    CT_EQ(r, 0);
    CT_EQ(m.format, 2);
    CT_EQ(m.ndevs, 2);
    CT_EQ(m.devs[0].settle_ms, 1500);
    CT_EQ(m.devs[0].pwr, HP_PWR_ONOFF);
    CT_EQ(m.devs[1].pwr, HP_PWR_TOGGLE);
    CT_CHECK(!strcmp(m.devs[0].name, "TV") && !strcmp(m.devs[1].name, "amp"));
    CT_EQ(m.ncmds, 4);
    CT_EQ(m.ninputs, 1);
    CT_EQ(m.acts[0].ndevs, 2);
    CT_EQ(m.acts[0].devs[0].input, 0);
    CT_EQ(m.acts[0].devs[1].input, -1);
    CT_CHECK(!strcmp(hp_map_activity_name(&m, 0), "Watch TV"));
    CT_CHECK(!strcmp(hp_map_activity_id(&m, 1), "music"));
    CT_CHECK(!strcmp(hp_map_activity_name(&m, 1), "music")); /* name defaults to id */

    hp_action_t a;
    /* per-activity layer wins, then top-level keys */
    CT_EQ(hp_map_lookup(&m, "Watch TV", 2, &a), 1);
    CT_EQ(a.type, HP_ACT_IR);
    CT_EQ(a.proto, HP_IR_RC5);
    CT_EQ(hp_map_lookup(&m, "music", 2, &a), 1);
    CT_EQ(a.type, HP_ACT_HA_EVENT);
    CT_EQ(hp_map_lookup(&m, "music", 4, &a), 1); /* device input resolved at load */
    CT_EQ(a.type, HP_ACT_IR);
    CT_EQ(a.cmd, 9);
    CT_EQ(hp_map_lookup(&m, NULL, 255, &a), 1);
    CT_EQ(a.proto, HP_IR_SAMSUNG32);
    CT_EQ(a.port_mask, 255); /* default */

    /* activity-start key action */
    CT_EQ(hp_key_handle(&m, &sinks, 1, 1, HP_KEY_ACTION_PRESS, NULL, NULL), 0);
    CT_EQ(hp_map_current(&m), 0);
    CT_EQ(hp_sim_count(&sim), 1); /* the activity's single step: vol_up */
    CT_EQ(hp_sim_rec(&sim, 0)->port_mask, 3);
    CT_EQ(hp_sim_rec(&sim, 0)->hz, 38000);
    CT_EQ(hp_key_handle(&m, &sinks, 1, 3, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* off */
    CT_EQ(hp_map_current(&m), -1);
    CT_EQ(hp_sim_count(&sim), 2); /* off step: tv power_off */
    CT_EQ(hp_sim_rec(&sim, 1)->port_mask, 255);
    CT_EQ(hp_key_handle(&m, &sinks, 1, 3, HP_KEY_ACTION_PRESS, NULL, NULL), 0); /* off when nothing runs */
    CT_EQ(hp_sim_count(&sim), 2);

    /* power_on of a toggle device resolves to the toggle code */
    const char *t = "{\"format\":2,\"devices\":[{\"id\":\"amp\",\"power\":{\"toggle\":{\"proto\":\"rc5\",\"addr\":1,\"cmd\":12}}}],"
                    "\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"amp\",\"command\":\"power_on\"}}}";
    CT_EQ(hp_map_load(&m, t, strlen(t)), 0);
    CT_EQ(hp_map_lookup(&m, NULL, 1, &a), 1);
    CT_EQ(a.cmd, 12);
    hp_map_free(&m);
}

static void test_each_ir_form_encodes(void) {
    setup();
    hp_map_t m;
    hp_map_init(&m);
    hp_map_set_out(&m, &out);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    hp_action_t a;
    uint32_t raw[HP_IR_MAX_DUR];
    hp_ir_code_t c;
    hp_ir_signal_t sig;
    for (int i = 0; i < m.ncmds; i++) {
        CT_EQ(hp_map_ir_code(&m, &m.cmds[i].act, raw, &c), 0);
        CT_EQ(hp_ir_encode(&c, 0, &sig), 0);
    }
    a = m.cmds[1].act; /* raw1 */
    CT_EQ(a.ir_kind, HP_IR_CODE_RAW);
    CT_EQ(hp_map_apply(&m, &a, &sinks), 0);
    CT_EQ(hp_sim_rec(&sim, 0)->n, 4);
    CT_EQ(hp_sim_rec(&sim, 0)->us[1], 4500);
    a = m.cmds[2].act; /* pr */
    CT_EQ(hp_map_apply(&m, &a, &sinks), 0);
    CT_EQ(hp_sim_rec(&sim, 1)->us[0], 631);
    hp_map_free(&m);
}

static const char *PRE = "{\"format\":2,\"devices\":[{\"id\":\"tv\",\"commands\":{\"ok\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2}}}],";

static void test_load_errors(void) {
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    const char *bad[] = {
        /* IR action forms: exactly one */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\"}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":1,\"cmd\":1,\"pronto\":\"0000 006d 0001 0000 0018 0018\"}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"raw\":{\"hz\":38000,\"us\":[1,2]},\"pronto\":\"0000 006d 0001 0000 0018 0018\"}}}",
        /* encoder rejects at load, not at key press */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":300,\"cmd\":1}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"proto\":\"rc5\",\"addr\":1,\"cmd\":200}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"proto\":\"warp\",\"addr\":1,\"cmd\":1}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"raw\":{\"hz\":38000,\"us\":[1,2,3]}}}}", /* odd */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"raw\":{\"hz\":10,\"us\":[1,2]}}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"raw\":{\"hz\":38000,\"us\":[0,2]}}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"pronto\":\"0000 006d 0005 0000 0018\"}}}", /* count */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"pronto\":\"zzzz\"}}}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"pronto\":[0,109,1,0,24,24]}}}", /* int array: format 1 only */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"proto\":\"nec\",\"addr\":1,\"cmd\":1,\"port_mask\":0}}}",
        /* key codes 0..255 */
        "{\"format\":2,\"keys\":{\"256\":{\"type\":\"none\"}}}",
        "{\"format\":2,\"keys\":{\"-1\":{\"type\":\"none\"}}}",
        "{\"format\":3,\"keys\":{}}",
        /* device refs */
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"tv\",\"command\":\"ok\"}}}", /* no devices */
        "{\"format\":2,\"devices\":[{\"id\":\"a\"},{\"id\":\"a\"}]}",
        "{\"format\":2,\"devices\":[{\"id\":\"a\",\"power\":{\"on\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":1}}}]}",
        "{\"format\":2,\"devices\":[{\"id\":\"a\",\"settle_ms\":70000}]}",
        "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"proto\":\"nec\",\"addr\":999,\"cmd\":1}}}]}",
        "{\"format\":2,\"activities\":[{\"id\":\"a\",\"devices\":[{\"device\":\"ghost\"}]}]}",
        "{\"format\":2,\"activities\":[{\"id\":\"a\"},{\"id\":\"a\"}]}",
        "{\"format\":2,\"activities\":[{\"id\":\"off\"}]}",
        "{\"format\":2,\"activities\":[{\"name\":\"no id\"}]}",
        "{\"format\":2,\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"ghost\"}}}",
        "{\"format\":2,\"activities\":[{\"id\":\"a\",\"steps\":[{\"type\":\"activity\",\"activity\":\"a\"}]}]}",
        /* format 1 may not use format 2 action types */
        "{\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"a\"}}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int r = hp_map_load(&m, bad[i], strlen(bad[i]));
        if (r >= 0) fprintf(stderr, "accepted: %s\n", bad[i]);
        CT_CHECK(r < 0);
        CT_CHECK(hp_map_last_error(&m)[0] != '\0');
        CT_EQ(hp_map_activity_count(&m), 2); /* old map intact */
        CT_EQ(m.format, 2);
    }
    const char *dev_unknown_cmd = "{\"format\":2,\"devices\":[{\"id\":\"tv\"}],"
                                  "\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"tv\",\"command\":\"nope\"}}}";
    CT_EQ(hp_map_load(&m, dev_unknown_cmd, strlen(dev_unknown_cmd)), -HP_E_INVAL);
    CT_CHECK(strstr(hp_map_last_error(&m), "nope") != NULL);
    char buf[512];
    snprintf(buf, sizeof buf, "%s\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"tv\",\"command\":\"ok\"}}}", PRE);
    CT_EQ(hp_map_load(&m, buf, strlen(buf)), 0); /* the PRE prefix itself is valid */
    CT_EQ(m.format, 2);
    hp_map_free(&m);
}

static char big[400000];

static void test_limits(void) {
    hp_map_t m;
    hp_map_init(&m);
    size_t n;
#define BEGIN(s) n = (size_t)snprintf(big, sizeof big, "%s", s)
#define ADD(...) n += (size_t)snprintf(big + n, sizeof big - n, __VA_ARGS__)
    BEGIN("{\"format\":2,\"devices\":[");
    for (int i = 0; i <= HP_MAP_MAX_DEVICES; i++) ADD("%s{\"id\":\"d%d\"}", i ? "," : "", i);
    ADD("]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    CT_CHECK(strstr(hp_map_last_error(&m), "devices") != NULL);

    BEGIN("{\"format\":2,\"devices\":[");
    for (int d = 0; d < 5; d++) {
        ADD("%s{\"id\":\"d%d\",\"commands\":{", d ? "," : "", d);
        for (int i = 0; i < 52; i++) ADD("%s\"c%d\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":%d}", i ? "," : "", i, i);
        ADD("}}");
    }
    ADD("]}"); /* 260 commands > 256 */
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);

    BEGIN("{\"format\":2,\"devices\":[{\"id\":\"d\",\"inputs\":{");
    for (int i = 0; i <= HP_MAP_MAX_INPUTS; i++) ADD("%s\"i%d\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":1}", i ? "," : "", i);
    ADD("}}]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);

    BEGIN("{\"format\":2,\"keys\":{");
    for (int i = 0; i <= HP_MAP_MAX_KEYS; i++) ADD("%s\"%d\":{\"type\":\"none\"}", i ? "," : "", i);
    ADD("}}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);

    BEGIN("{\"format\":2,\"devices\":[{\"id\":\"d\"}],\"activities\":[{\"id\":\"a\",\"devices\":[");
    for (int i = 0; i <= HP_MAP_MAX_ACT_DEVS; i++) ADD("%s{\"device\":\"d\"}", i ? "," : "");
    ADD("]}]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);

    /* raw longer than the encoder limit, and pronto longer than the per-action cap */
    BEGIN("{\"format\":2,\"keys\":{\"1\":{\"raw\":{\"hz\":38000,\"us\":[1");
    for (int i = 0; i < HP_IR_MAX_DUR; i++) ADD(",1");
    ADD("]},\"type\":\"ir\"}}}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    BEGIN("{\"format\":2,\"keys\":{\"1\":{\"type\":\"ir\",\"pronto\":\"0000 006d 0000 0000");
    for (int i = 0; i < HP_MAP_MAX_PRONTO; i++) ADD(" 0001");
    ADD("\"}}}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);

    /* the shared IR pool: 40 x 512 raw durations = 20480 > 16384 */
    BEGIN("{\"format\":2,\"devices\":[{\"id\":\"d\",\"commands\":{");
    for (int c = 0; c < 40; c++) {
        ADD("%s\"c%d\":{\"raw\":{\"hz\":38000,\"us\":[1", c ? "," : "", c);
        for (int i = 1; i < 512; i++) ADD(",1");
        ADD("]}}");
    }
    ADD("}}]}");
    CT_EQ(hp_map_load(&m, big, n), -HP_E_TOOBIG);
    CT_CHECK(strstr(hp_map_last_error(&m), "pool") != NULL);
    CT_EQ(hp_map_load(&m, big, HP_MAP_MAX_JSON + 1), -HP_E_TOOBIG);
    hp_map_free(&m);
}

static void test_format1_int_pronto_via_encoder(void) {
    hp_map_t m;
    hp_map_init(&m);
    const char *ok = "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,109,1,0,24,24]}}}";
    CT_EQ(hp_map_load(&m, ok, strlen(ok)), 0);
    CT_EQ(m.format, 1);
    const char *junk = "{\"keys\":{\"1\":{\"type\":\"ir\",\"port_mask\":1,\"pronto\":[0,109,0,0]}}}"; /* no bursts */
    CT_EQ(hp_map_load(&m, junk, strlen(junk)), -HP_E_INVAL);
    const char *fmt1 = "{\"format\":1,\"keys\":{\"70\":{\"type\":\"none\"}}}";
    CT_EQ(hp_map_load(&m, fmt1, strlen(fmt1)), 0);
    hp_map_free(&m);
}

static void test_cache_and_hash(void) {
    char path[] = "/tmp/hp_cfg2_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    hp_map_t m, m2;
    hp_map_init(&m);
    hp_map_init(&m2);
    char h1[9], h2[9];
    CT_CHECK(!hp_map_hash_hex(&m, h1));
    CT_EQ(h1[0], 0);
    CT_EQ(hp_map_load(&m, CFG, strlen(CFG)), 0);
    CT_CHECK(hp_map_hash_hex(&m, h1));
    CT_EQ(strlen(h1), 8);
    CT_EQ(hp_map_save_cache(&m, path), 0);
    CT_EQ(hp_map_load_cache(&m2, path), 0);
    CT_EQ(m2.format, 2);
    CT_EQ(m2.ndevs, 2);
    CT_CHECK(hp_map_hash_hex(&m2, h2));
    CT_CHECK(!strcmp(h1, h2)); /* identical config -> identical hash */
    const char *other = "{\"format\":2,\"keys\":{\"1\":{\"type\":\"none\"}}}";
    CT_EQ(hp_map_load(&m2, other, strlen(other)), 0);
    hp_map_hash_hex(&m2, h2);
    CT_CHECK(strcmp(h1, h2) != 0);
    unlink(path);
    hp_map_free(&m);
    hp_map_free(&m2);
}

/* Reload keeps the current activity by id (format 2), so a rename does not drop it. */
static void test_reload_matches_by_id(void) {
    const char *v1 = "{\"format\":2,\"devices\":[],\"activities\":[{\"id\":\"x\",\"name\":\"Old\"},{\"id\":\"y\",\"name\":\"Other\"}]}";
    const char *v2 = "{\"format\":2,\"devices\":[],\"activities\":[{\"id\":\"y\",\"name\":\"Old\"},{\"id\":\"x\",\"name\":\"Renamed\"}]}";
    const char *v3 = "{\"format\":2,\"devices\":[],\"activities\":[{\"id\":\"z\",\"name\":\"Old\"}]}";
    hp_map_t m;
    hp_map_init(&m);
    hp_sinks_t sinks = {0};
    CT_EQ(hp_map_load(&m, v1, strlen(v1)), 0);
    CT_EQ(hp_activity_set(&m, "Old", &sinks), 0);
    CT_EQ(hp_map_load(&m, v2, strlen(v2)), 0);
    CT_EQ(hp_map_current(&m), 1); /* id x, now named Renamed; NOT the other activity now called Old */
    CT_EQ(hp_map_load(&m, v3, strlen(v3)), 0);
    CT_EQ(hp_map_current(&m), -1); /* id gone, even though the name "Old" exists */
    hp_map_free(&m);
}

int main(void) {
    CT_RUN(test_load_and_resolve);
    CT_RUN(test_each_ir_form_encodes);
    CT_RUN(test_load_errors);
    CT_RUN(test_limits);
    CT_RUN(test_format1_int_pronto_via_encoder);
    CT_RUN(test_cache_and_hash);
    CT_RUN(test_reload_matches_by_id);
    return CT_DONE();
}
