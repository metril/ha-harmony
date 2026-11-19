/* Config memory budget: large configs load within it (CPU time and peak memory asserted, never wall
 * clock), over-budget and failed loads leave the old map untouched, every allocation of a load can
 * fail cleanly, the estimate is stable, and config_chunk assembles the same config as set_config. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "ctest_assert.h"
#include "hub/engine/mapping.h"
#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/net/jsonrpc.h"

static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

/* `ndev` devices sharing `ncmd` protocol commands in total; every `raw_every`-th command is a learned raw code. */
static char *gen(size_t ndev, size_t ncmd, size_t raw_every, size_t *len) {
    size_t cap = ncmd * 700 + ndev * 80 + 256;
    char *b = malloc(cap);
    size_t n = (size_t)snprintf(b, cap, "{\"format\":2,\"devices\":[");
    size_t per = (ncmd + ndev - 1) / ndev, done = 0;
    for (size_t d = 0; d < ndev; d++) {
        n += (size_t)snprintf(b + n, cap - n, "%s{\"id\":\"dev%zu\",\"commands\":{", d ? "," : "", d);
        for (size_t i = 0; i < per && done < ncmd; i++, done++) {
            if (raw_every && done % raw_every == 0) {
                n += (size_t)snprintf(b + n, cap - n, "%s\"command_%zu\":{\"raw\":{\"hz\":38000,\"us\":[9000,4500", i ? "," : "", done);
                for (int k = 0; k < 30; k++) n += (size_t)snprintf(b + n, cap - n, ",560,560");
                n += (size_t)snprintf(b + n, cap - n, "]}}");
            } else {
                n += (size_t)snprintf(b + n, cap - n, "%s\"command_%zu\":{\"proto\":\"nec\",\"addr\":%zu,\"cmd\":%zu}", i ? "," : "",
                                      done, d, done % 256);
            }
        }
        n += (size_t)snprintf(b + n, cap - n, "}}");
    }
    n += (size_t)snprintf(b + n, cap - n, "],\"keys\":{\"1\":{\"type\":\"device\",\"device\":\"dev0\",\"command\":\"command_1\"}}}");
    *len = n;
    return b;
}

static void test_big_loads(void) {
    size_t n;
    char *cfg = gen(40, 10000, 0, &n);
    hp_map_t m;
    hp_map_init(&m);
    double t0 = cpu_s();
    int lr = hp_map_load(&m, cfg, n);
    if (lr) fprintf(stderr, "err: %s\n", hp_map_last_error(&m));
    CT_EQ(lr, 0);
    double dt = cpu_s() - t0;
    CT_EQ(m.ncmds, 10000);
    fprintf(stderr, "load 10000 commands: %.3f s CPU, est %zu KiB\n", dt, m.est_bytes / 1024);
    CT_CHECK(dt < 3.0);
    CT_CHECK(m.est_bytes <= hp_map_budget(&m));
    t0 = cpu_s();
    char name[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(name, sizeof name, "command_%d", i);
        CT_CHECK(hp_map_find_cmd(&m, i / 250, name) == i);
    }
    CT_CHECK(cpu_s() - t0 < 1.0);
    CT_EQ(hp_map_find_cmd(&m, 0, "nope"), -1);
    hp_map_free(&m);
    free(cfg);

    /* 50,000 commands: over the hub's default budget, within a larger device's */
    cfg = gen(200, 50000, 0, &n);
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, cfg, n), -HP_E_TOOBIG);
    CT_CHECK(strstr(hp_map_last_error(&m), "KiB of hub memory") != NULL);
    hp_map_set_budget(&m, 64u * 1024 * 1024);
    t0 = cpu_s();
    CT_EQ(hp_map_load(&m, cfg, n), 0);
    CT_CHECK(cpu_s() - t0 < 15.0);
    CT_EQ(m.ncmds, 50000);
    t0 = cpu_s();
    for (int i = 0; i < 50000; i++) {
        snprintf(name, sizeof name, "command_%d", i);
        CT_CHECK(hp_map_find_cmd(&m, i / 250, name) == i);
    }
    CT_CHECK(cpu_s() - t0 < 2.0); /* hashed: nothing linear per lookup */
    hp_map_free(&m);
    free(cfg);
}

/* Peak resident set so far (VmHWM): per mm, so unlike ru_maxrss it restarts at exec. */
static size_t hwm_bytes(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    size_t kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmHWM:", 6)) kb = (size_t)atol(line + 6);
    if (f) fclose(f);
    return kb * 1024;
}

/* Peak resident growth of a load, measured in a fresh process (a forked child inherits the parent's
 * freed-but-resident heap and would hide it). `mix`: every 7th command is a learned raw code. */
static int peak_child(size_t ncmd, size_t mix, size_t *grown_out, size_t *est_out, size_t *text_out) {
    size_t n;
    char *cfg = gen(40, ncmd, mix, &n);
    size_t before = hwm_bytes();
    hp_map_t m;
    hp_map_init(&m);
    int rc = hp_map_load(&m, cfg, n);
    *grown_out = hwm_bytes() - before;
    *est_out = m.est_bytes;
    *text_out = n;
    return rc;
}

static const char *SELF;

static void test_peak_memory(void) {
#if defined(__SANITIZE_ADDRESS__) || UINTPTR_MAX <= 0xffffffffu
    return; /* ASan redzones, or a qemu-emulated target: RSS is not the target's; the plain 64-bit host build asserts it */
#endif
    for (int mix = 0; mix <= 7; mix += 7) {
        char arg[16];
        snprintf(arg, sizeof arg, "%d", mix);
        pid_t pid = fork();
        CT_CHECK(pid >= 0);
        if (pid == 0) {
            execl(SELF, SELF, "peak", arg, (char *)NULL);
            _exit(99);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
}

static const char *SMALL = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2},"
                           "\"y\":{\"raw\":{\"hz\":38000,\"us\":[9000,4500,560,560]}}}}]}";

/* Golden numbers: tests/py/test_limits.py asserts the same ones from the Python mirror. */
static void test_estimate_golden(void) {
    size_t est = 0;
    CT_EQ(hp_map_estimate(SMALL, strlen(SMALL), &est), 0);
    CT_EQ(strlen(SMALL), 135);
    CT_EQ(est, 3 * 135 + 18 * HP_MAP_NODE_BYTES + HP_MAP_DEV_BYTES + 2 * HP_MAP_CMD_BYTES + 4 * HP_MAP_WORD_BYTES);
    CT_EQ(est, 3 * 135 + 18 * 80 + 224 + 240 + 8);
    CT_EQ(hp_map_estimate("{", 1, &est), -HP_E_INVAL);
}

/* A rejected load (bad config, over budget, any allocation failing) keeps the old map byte for byte. */
static void test_failed_load_keeps_old_map(void) {
    size_t n;
    char *cfg = gen(4, 400, 5, &n);
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, SMALL, strlen(SMALL)), 0);
    char h0[9], h1[9];
    hp_map_hash_hex(&m, h0);
    uint32_t nc = m.ncmds;
    const char *bad = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"proto\":\"nope\"}}}]}";
    CT_EQ(hp_map_load(&m, bad, strlen(bad)), -HP_E_INVAL);
    hp_map_set_budget(&m, 2048);
    CT_EQ(hp_map_load(&m, cfg, n), -HP_E_TOOBIG);
    hp_map_set_budget(&m, 0);
    hp_map_hash_hex(&m, h1);
    CT_CHECK(!strcmp(h0, h1));
    CT_EQ(m.ncmds, nc);
    CT_EQ(hp_map_find_cmd(&m, 0, "y"), 1);

    /* every allocation of the load fails in turn: a clean error each time, then success */
    int fails = 0;
    for (unsigned k = 1; k < 200; k++) {
        hp_map_test_fail_alloc(k);
        int rc = hp_map_load(&m, cfg, n);
        if (rc == 0) {
            hp_map_hash_hex(&m, h1);
            CT_CHECK(strcmp(h0, h1) != 0); /* the new config is in */
            break;
        }
        fails++;
        CT_CHECK(rc < 0 && hp_map_last_error(&m)[0]);
        hp_map_hash_hex(&m, h1);
        CT_CHECK(!strcmp(h0, h1)); /* ...and otherwise the old one */
        CT_EQ(m.ncmds, nc);
    }
    hp_map_test_fail_alloc(0);
    CT_CHECK(fails >= 6);
    hp_map_free(&m);
    free(cfg);
}

static unsigned cj_fail_at, cj_n;
static void *cj_malloc(size_t sz) { return (cj_fail_at && ++cj_n == cj_fail_at) ? NULL : malloc(sz); }

/* The parse tree's allocations failing is as clean (cJSON allocations go through hooks). */
static void test_parse_tree_alloc_failure(void) {
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, SMALL, strlen(SMALL)), 0);
    char h0[9], h1[9];
    hp_map_hash_hex(&m, h0);
    const char *other = "{\"format\":2,\"devices\":[{\"id\":\"z\",\"commands\":{\"q\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2}}}]}";
    cJSON_Hooks hk = {cj_malloc, free};
    cJSON_InitHooks(&hk);
    for (unsigned k = 1; k < 80; k++) {
        cj_fail_at = k;
        cj_n = 0;
        int rc = hp_map_load(&m, other, strlen(other));
        cj_fail_at = 0;
        hp_map_hash_hex(&m, h1);
        if (rc == 0) {
            CT_CHECK(strcmp(h0, h1) != 0);
            break;
        }
        CT_CHECK(!strcmp(h0, h1));
    }
    cJSON_InitHooks(NULL);
    hp_map_free(&m);
}

/* ---- config_chunk through the dispatcher */

static hub_ctx_t H;
static hp_handler_tbl_t tbl = {.token = "t", .hub_id = "h"};
static char rep[8192];
static size_t rl;

static int send_chunk(hp_session_t *s, uint32_t seq, uint32_t total, const char *data, size_t dl) {
    char *msg = malloc(2 * dl + 256);
    int k = snprintf(msg, 2 * dl + 256, "{\"v\":1,\"id\":%u,\"t\":\"config_chunk\",\"d\":{\"seq\":%u,\"total\":%u,\"data\":\"", seq + 1, seq,
                     total);
    for (size_t i = 0; i < dl; i++) {
        if (data[i] == '"') msg[k++] = '\\';
        msg[k++] = data[i];
    }
    k += snprintf(msg + k, 8, "\"}}");
    memset(rep, 0, sizeof rep);
    int rc = hp_dispatch(msg, (size_t)k, &tbl, s, rep, sizeof rep, &rl);
    free(msg);
    return rc;
}

static void test_chunked_config(void) {
    hub_ctx_init(&H);
    hub_install(&tbl, &H);
    hp_session_t s = {.authed = true};
    size_t n;
    char *cfg = gen(4, 3000, 0, &n);
    const size_t step = 7000;
    uint32_t seq = 0;
    for (size_t off = 0; off < n; off += step, seq++) {
        size_t dl = n - off < step ? n - off : step;
        CT_EQ(send_chunk(&s, seq, (uint32_t)n, cfg + off, dl), HP_DISPATCH_OK);
        CT_CHECK(strstr(rep, "\"ok\":true"));
        if (off + step < n) CT_CHECK(!strstr(rep, "config_hash"));
    }
    CT_CHECK(strstr(rep, "config_hash")); /* the last chunk answers like set_config */
    CT_EQ(H.map.ncmds, 3000);
    /* out of order / wrong total: refused, nothing half-applied */
    CT_EQ(send_chunk(&s, 0, (uint32_t)n, cfg, 1000), HP_DISPATCH_OK);
    CT_EQ(send_chunk(&s, 2, (uint32_t)n, cfg + 1000, 1000), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "\"ok\":false") && strstr(rep, "out of order"));
    CT_EQ(H.map.ncmds, 3000);
    CT_CHECK(H.chunk_buf == NULL);
    /* a total over the budget is refused on the first chunk */
    CT_EQ(send_chunk(&s, 0, 100u * 1024 * 1024, cfg, 10), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "\"ok\":false") && strstr(rep, "KiB"));
    /* hello_ack carries the limits */
    memset(rep, 0, sizeof rep);
    hp_session_t anon = {0};
    const char *hello = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"t\"}}";
    CT_EQ(hp_dispatch(hello, strlen(hello), &tbl, &anon, rep, sizeof rep, &rl), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "\"limits\"") && strstr(rep, "\"budget\":8388608") && strstr(rep, "\"max_devices\":255"));
    hub_shutdown(&H);
    free(cfg);
}

int main(int argc, char **argv) {
    SELF = argv[0];
    if (argc > 2 && !strcmp(argv[1], "peak")) { /* child of test_peak_memory */
        size_t grown = 0, est = 0, text = 0;
        int rc = peak_child(argc > 3 ? (size_t)atol(argv[3]) : 5000, (size_t)atoi(argv[2]), &grown, &est, &text);
        /* The estimate models the 32-bit hub; a 64-bit host's pointers (cJSON nodes) are twice as wide. The received
         * text is already resident before the load, so it is not part of the growth. */
        size_t limit = (est - text) * (sizeof(void *) == 8 ? 3 : 2) / 2;
        fprintf(stderr, "peak: load rc=%d grew %zu B, estimate %zu B (limit %zu)\n", rc, grown, est, limit);
        return rc == 0 && grown <= limit ? 0 : 1;
    }
    CT_RUN(test_estimate_golden);
    CT_RUN(test_big_loads);
    CT_RUN(test_peak_memory);
    CT_RUN(test_failed_load_keeps_old_map);
    CT_RUN(test_parse_tree_alloc_failure);
    CT_RUN(test_chunked_config);
    return CT_DONE();
}
