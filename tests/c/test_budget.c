/* Config memory budget: large configs load within it (CPU time and peak memory asserted, never wall
 * clock), over-budget and failed loads leave the old map untouched, every allocation of a load can
 * fail cleanly, the estimate is stable, and config_chunk assembles the same config as set_config. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

    /* 50,000 commands: the tables are ~6 MB, within the default budget (no parse tree to pay for) */
    cfg = gen(200, 50000, 0, &n);
    hp_map_init(&m);
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

    /* 200,000 commands: over the budget, rejected before the tables are allocated */
    cfg = gen(800, 200000, 0, &n);
    hp_map_init(&m);
    CT_EQ(hp_map_load(&m, cfg, n), -HP_E_TOOBIG);
    CT_CHECK(strstr(hp_map_last_error(&m), "KiB of hub memory") != NULL);
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

/* Boundary configs (protocol, Pronto, raw): tests/py/test_limits.py extracts the BOUNDARY_* literals and asserts the
 * same estimates and the same accept/refuse edge from the Python mirror (the editor's pre-check). */
static const char *BOUNDARY_PROTO = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":2},"
                                    "\"y\":{\"proto\":\"nec\",\"addr\":1,\"cmd\":3},\"z\":{\"proto\":\"rc5\",\"addr\":5,\"cmd\":9}}}]}";
static const char *BOUNDARY_PRONTO = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"pronto\":\"0000 006d 0001 0000 0018 0018\"},"
                                     "\"y\":{\"pronto\":\"0000 006d 0002 0000 0018 0018 0018 0018\"}}}]}";
static const char *BOUNDARY_RAW = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"raw\":{\"hz\":38000,\"us\":[9000,4500,560,560,560,1680]}},"
                                  "\"y\":{\"raw\":{\"hz\":38000,\"us\":[9000,4500]}}}},{\"id\":\"b\",\"commands\":{\"z\":{\"proto\":\"nec\",\"addr\":2,\"cmd\":1}}}]}";

static void test_budget_boundary(void) {
    const char *cfg[] = {BOUNDARY_PROTO, BOUNDARY_PRONTO, BOUNDARY_RAW};
    for (size_t i = 0; i < 3; i++) {
        size_t est = 0;
        CT_EQ(hp_map_estimate(cfg[i], strlen(cfg[i]), &est), 0);
        fprintf(stderr, "boundary %zu: est %zu\n", i, est);
        hp_map_t m;
        hp_map_init(&m);
        hp_map_set_budget(&m, est); /* exactly the estimate: accepted */
        CT_EQ(hp_map_load(&m, cfg[i], strlen(cfg[i])), 0);
        hp_map_free(&m);
        hp_map_init(&m);
        hp_map_set_budget(&m, est - 1); /* one byte less: refused */
        CT_CHECK(hp_map_load(&m, cfg[i], strlen(cfg[i])) < 0);
        hp_map_free(&m);
    }
}

/* Golden numbers: tests/py/test_limits.py asserts the same ones from the Python mirror. */
static void test_estimate_golden(void) {
    size_t est = 0;
    CT_EQ(hp_map_estimate(SMALL, strlen(SMALL), &est), 0);
    CT_EQ(strlen(SMALL), 135);
    CT_EQ(est, HP_MAP_SLACK_BYTES + HP_MAP_DEV_BYTES + 2 * HP_MAP_CMD_BYTES + 4 * HP_MAP_WORD_BYTES);
    CT_EQ(est, 1573336);
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
    CT_CHECK(H.xfer == NULL);
    /* hello_ack carries the limits */
    memset(rep, 0, sizeof rep);
    hp_session_t anon = {0};
    const char *hello = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"t\"}}";
    CT_EQ(hp_dispatch(hello, strlen(hello), &tbl, &anon, rep, sizeof rep, &rl), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "\"limits\"") && strstr(rep, "\"budget\":8388608") && strstr(rep, "\"max_devices\":255"));
    hub_shutdown(&H);
    hp_map_free(&H.map);
    free(cfg);
}

static bool push(hp_session_t *s, const char *cfg, size_t n) { /* config_chunk transfer; true = loaded */
    uint32_t seq = 0;
    for (size_t off = 0; off < n; off += 7000, seq++) {
        size_t dl = n - off < 7000 ? n - off : 7000;
        send_chunk(s, seq, (uint32_t)n, cfg + off, dl);
        if (!strstr(rep, "\"ok\":true")) return false;
    }
    return true;
}

static size_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (size_t)st.st_size : 0;
}

/* The received text becomes the cache file; a config that does not load, is over budget, or cannot be
 * written (full flash: simulated by a file size limit) leaves the previous map AND cache file untouched. */
static void test_cache_is_all_or_nothing(void) {
    char path[] = "/tmp/hp_budget_cache_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    unlink(path);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    hub_ctx_init(&H);
    hub_install(&tbl, &H);
    H.cache_path = path;
    hp_session_t s = {.authed = true};
    size_t n, n2;
    char *good = gen(4, 3000, 0, &n), *other = gen(4, 2000, 0, &n2);
    CT_CHECK(push(&s, good, n));
    size_t sz = file_size(path);
    CT_CHECK(sz > n);
    char hash[9], h2[9];
    hp_map_hash_hex(&H.map, hash);
    CT_CHECK(access(tmp, F_OK) != 0); /* the temp file is gone */
    /* it is a valid cache: a fresh map loads it by streaming */
    hp_map_t boot;
    hp_map_init(&boot);
    CT_EQ(hp_map_load_cache(&boot, path), 0);
    CT_EQ(boot.ncmds, 3000);
    hp_map_free(&boot);

    const char *bad = "{\"format\":2,\"devices\":[{\"id\":\"a\",\"commands\":{\"x\":{\"proto\":\"nope\"}}}]}";
    CT_CHECK(!push(&s, bad, strlen(bad)));
    CT_CHECK(strstr(rep, "protocol"));
    size_t keep = hp_map_budget(&H.map);
    hp_map_set_budget(&H.map, 1600u * 1024); /* below slack + 2000 commands */
    CT_CHECK(!push(&s, other, n2));
    CT_CHECK(strstr(rep, "KiB of hub memory"));
    hp_map_set_budget(&H.map, keep);
    /* a full flash: writes fail partway */
    struct rlimit old, lim = {4000, 0};
    getrlimit(RLIMIT_FSIZE, &old);
    lim.rlim_max = old.rlim_max;
    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &lim);
    CT_CHECK(!push(&s, other, n2));
    setrlimit(RLIMIT_FSIZE, &old);
    CT_CHECK(strstr(rep, "flash") || strstr(rep, "space"));
    CT_EQ(H.map.ncmds, 3000);
    hp_map_hash_hex(&H.map, h2);
    CT_CHECK(!strcmp(hash, h2));
    CT_EQ(file_size(path), sz);
    CT_CHECK(access(tmp, F_OK) != 0);
    /* and a good one afterwards replaces both */
    CT_CHECK(push(&s, other, n2));
    CT_EQ(H.map.ncmds, 2000);
    CT_CHECK(file_size(path) != sz);
    hub_shutdown(&H);
    hp_map_free(&H.map);
    unlink(path);
    free(good);
    free(other);
}

/* ---- review fixes: boot load ignores the budget, stale .new, flash reserve, overflow, dup sections, xfer lifetime */

static uint32_t g_now;
static uint32_t fake_clock(void *ctx) { (void)ctx; return g_now; }

static void test_boot_cache_ignores_budget(void) {
    char path[] = "/tmp/hp_budget_boot_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    unlink(path);
    hub_ctx_init(&H);
    hub_install(&tbl, &H);
    H.cache_path = path;
    hp_session_t s = {.authed = true};
    size_t n;
    char *cfg = gen(4, 3000, 0, &n);
    CT_CHECK(push(&s, cfg, n));
    hub_shutdown(&H);
    hp_map_free(&H.map);
    /* restart with a much smaller budget (less free memory this time): the accepted cache still loads */
    hp_map_t boot;
    hp_map_init(&boot);
    hp_map_set_budget(&boot, 256u * 1024);
    CT_EQ(hp_map_load_cache(&boot, path), 0);
    CT_EQ(boot.ncmds, 3000);
    CT_EQ(hp_map_budget(&boot), 256u * 1024); /* the budget is kept for new pushes */
    CT_CHECK(hp_map_load(&boot, cfg, n) < 0);  /* ...and applies to them */
    hp_map_free(&boot);
    unlink(path);
    free(cfg);
}

static void test_boot_cache_refused_far_over_budget(void) {
    char path[] = "/tmp/hp_budget_big_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    unlink(path);
    hub_ctx_init(&H);
    hub_install(&tbl, &H);
    H.cache_path = path;
    hp_session_t s = {.authed = true};
    size_t n;
    char *cfg = gen(4, 3000, 0, &n);
    CT_CHECK(push(&s, cfg, n));
    hub_shutdown(&H);
    hp_map_free(&H.map);
    hp_map_t boot;
    hp_map_init(&boot);
    hp_map_set_budget(&boot, 256u * 1024);
    CT_CHECK(n < 4u * 256 * 1024); /* within 4x: loads (see test_boot_cache_ignores_budget) */
    CT_EQ(hp_map_load_cache(&boot, path), 0);
    hp_map_free(&boot);
    hp_map_init(&boot);
    hp_map_set_budget(&boot, n / 4 - 1); /* the text is now above 4x the budget: refused, nothing parsed */
    CT_EQ(hp_map_load_cache(&boot, path), -HP_E_TOOBIG);
    CT_EQ(boot.ncmds, 0);
    hp_map_free(&boot);
    unlink(path);
    free(cfg);
}

static void test_stale_new_file_removed(void) {
    char path[] = "/tmp/hp_budget_stale_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    unlink(path);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    fputs("leftover", f);
    fclose(f);
    hp_map_t m;
    hp_map_init(&m);
    CT_EQ(hp_map_load_cache(&m, path), -HP_E_NOENT);
    CT_CHECK(access(tmp, F_OK) != 0); /* startup load removed it */
    f = fopen(tmp, "w");
    fputs("leftover", f);
    fclose(f);
    hp_cachew_t *w = NULL;
    hp_cache_set_reserve(SIZE_MAX); /* statvfs check fails, but the stale file is gone first */
    CT_EQ(hp_cachew_open(&w, path, 10), -HP_E_TOOBIG);
    CT_CHECK(access(tmp, F_OK) != 0);
    hp_cache_set_reserve(0);
    hp_map_free(&m);
}

static void test_flash_reserve_and_overflow(void) {
    char path[] = "/tmp/hp_budget_flash_XXXXXX";
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    close(fd);
    unlink(path);
    CT_CHECK(HP_CACHE_RESERVE_DEFAULT >= 2u * 1024 * 1024); /* default never zero */
    hp_cache_set_reserve(0);
    size_t raw = hp_cache_free_bytes(path);
    CT_CHECK(raw != SIZE_MAX && raw > 0);
    hp_cache_set_reserve(1024);
    CT_CHECK(hp_cache_free_bytes(path) <= raw);
    CT_EQ(hp_cache_reserve(), 1024);
    hp_cache_set_reserve(SIZE_MAX); /* more than the partition: reported as 0 free, not wrapped */
    CT_EQ(hp_cache_free_bytes(path), 0);
    hp_cachew_t *w = NULL;
    CT_EQ(hp_cachew_open(&w, path, 1), -HP_E_TOOBIG);
    hp_cache_set_reserve(0);
    /* totals whose header/trailer sums would wrap a 32-bit size_t are refused before any arithmetic */
    size_t huge[] = {SIZE_MAX, SIZE_MAX - 8, SIZE_MAX - 12, SIZE_MAX - 13};
    for (size_t i = 0; i < sizeof huge / sizeof *huge; i++) CT_EQ(hp_cachew_open(&w, path, huge[i]), -HP_E_TOOBIG);
    CT_CHECK(w == NULL);
    /* through the wire: a total above the budget is refused up front (flash is not touched) */
    hub_ctx_init(&H);
    hub_install(&tbl, &H);
    H.cache_path = path;
    hp_session_t s = {.authed = true};
    CT_EQ(send_chunk(&s, 0, 0xFFFFFFF0u, "{", 1), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "\"ok\":false") && strstr(rep, "hub memory"));
    CT_CHECK(H.xfer == NULL);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    CT_CHECK(access(tmp, F_OK) != 0);
    /* the flash message and limits.flash_free both use the reserve */
    hp_cache_set_reserve(SIZE_MAX);
    CT_EQ(send_chunk(&s, 0, 100, "{", 1), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "of flash"));
    { /* nothing left is reported as such, not as "unknown" (a bare flash_free 0) */
        hp_session_t anon = {0};
        const char *hello = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"t\"}}";
        memset(rep, 0, sizeof rep);
        CT_EQ(hp_dispatch(hello, strlen(hello), &tbl, &anon, rep, sizeof rep, &rl), HP_DISPATCH_OK);
        CT_CHECK(strstr(rep, "\"flash_free\":0") && strstr(rep, "\"flash_low\":true"));
    }
    hp_cache_set_reserve(0);
    memset(rep, 0, sizeof rep);
    {
        hp_session_t anon = {0};
        const char *hello = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"t\"}}";
        CT_EQ(hp_dispatch(hello, strlen(hello), &tbl, &anon, rep, sizeof rep, &rl), HP_DISPATCH_OK);
        CT_CHECK(!strstr(rep, "flash_low"));
    }
    hub_shutdown(&H);
    hp_map_free(&H.map);
}

static void test_duplicate_top_level_sections(void) {
    const char *keys[] = {"keys", "activities", "format", "devices"};
    for (size_t k = 0; k < 4; k++) {
        char cfg[256];
        const char *v = k == 2 ? "2" : k == 1 ? "[]" : k == 3 ? "[]" : "{}";
        int n = snprintf(cfg, sizeof cfg, "{\"format\":2,\"%s\":%s,\"%s\":%s}", keys[k], v, keys[k], v);
        if (k == 2) n = snprintf(cfg, sizeof cfg, "{\"format\":2,\"format\":2}");
        hp_map_t m;
        hp_map_init(&m);
        CT_CHECK(hp_map_load(&m, cfg, (size_t)n) < 0);
        CT_CHECK(strstr(hp_map_last_error(&m), "duplicate"));
        hp_map_free(&m);
    }
    /* many duplicates of a retained section: refused at the second, never accumulated */
    size_t cap = 4u * 1024 * 1024, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n, "{\"format\":2,\"devices\":[]");
    while (n + 100 < cap) n += (size_t)snprintf(b + n, cap - n, ",\"keys\":{\"1\":{\"type\":\"activity\",\"activity\":\"off\"}}");
    n += (size_t)snprintf(b + n, cap - n, "}");
    hp_map_t m;
    hp_map_init(&m);
    CT_CHECK(hp_map_load(&m, b, n) < 0);
    CT_CHECK(strstr(hp_map_last_error(&m), "duplicate"));
    hp_map_free(&m);
    free(b);
    /* the retained sections are counted in the estimate */
    const char *a = "{\"format\":2,\"devices\":[]}";
    const char *c = "{\"format\":2,\"devices\":[],\"activities\":[{\"id\":\"x\",\"name\":\"X\",\"steps\":[],\"off\":[]}]}";
    size_t e1 = 0, e2 = 0;
    CT_EQ(hp_map_estimate(a, strlen(a), &e1), 0);
    CT_EQ(hp_map_estimate(c, strlen(c), &e2), 0);
    CT_CHECK(e2 > e1);
}

static void test_xfer_dropped_on_close_and_idle(void) {
    hub_ctx_init(&H);
    hub_set_clock(&H, fake_clock, NULL);
    hub_install(&tbl, &H);
    g_now = 1000;
    hp_session_t a = {.authed = true}, b = {.authed = true};
    size_t n;
    char *cfg = gen(2, 1000, 0, &n);
    CT_EQ(send_chunk(&a, 0, (uint32_t)n, cfg, 1000), HP_DISPATCH_OK);
    CT_CHECK(H.xfer != NULL && H.chunk_owner == &a);
    hub_session_closed(&H, &b); /* someone else's close does not touch it */
    CT_CHECK(H.xfer != NULL);
    hub_session_closed(&H, &a);
    CT_CHECK(H.xfer == NULL && H.chunk_owner == NULL);
    /* idle: dropped once 60 s pass without a chunk, kept before; each chunk restarts the clock */
    CT_EQ(send_chunk(&a, 0, (uint32_t)n, cfg, 1000), HP_DISPATCH_OK);
    g_now += 59000;
    hub_run_timers(&H);
    CT_CHECK(H.xfer != NULL);
    CT_EQ(send_chunk(&a, 1, (uint32_t)n, cfg + 1000, 1000), HP_DISPATCH_OK);
    g_now += 59000;
    hub_run_timers(&H);
    CT_CHECK(H.xfer != NULL);
    CT_CHECK(hub_next_timeout_ms(&H) >= 0 && hub_next_timeout_ms(&H) <= 1000);
    g_now += 1001;
    hub_run_timers(&H);
    CT_CHECK(H.xfer == NULL && H.chunk_owner == NULL);
    /* a later chunk of the dropped transfer is refused, a fresh one starts */
    CT_EQ(send_chunk(&a, 2, (uint32_t)n, cfg + 2000, 1000), HP_DISPATCH_OK);
    CT_CHECK(strstr(rep, "out of order"));
    hub_shutdown(&H);
    hp_map_free(&H.map);
    free(cfg);
}

int main(int argc, char **argv) {
    SELF = argv[0];
    if (argc > 2 && !strcmp(argv[1], "peak")) { /* child of test_peak_memory */
        size_t grown = 0, est = 0, text = 0;
        int rc = peak_child(argc > 3 ? (size_t)atol(argv[3]) : 5000, (size_t)atoi(argv[2]), &grown, &est, &text);
        /* tables + slack; the text (a caller's buffer here) was resident before the load and no tree of it is built */
        size_t limit = est;
        (void)text;
        fprintf(stderr, "peak: load rc=%d grew %zu B, estimate %zu B (limit %zu)\n", rc, grown, est, limit);
        return rc == 0 && grown <= limit ? 0 : 1;
    }
    CT_RUN(test_estimate_golden);
    CT_RUN(test_budget_boundary);
    CT_RUN(test_big_loads);
    CT_RUN(test_peak_memory);
    CT_RUN(test_failed_load_keeps_old_map);
    CT_RUN(test_parse_tree_alloc_failure);
    CT_RUN(test_chunked_config);
    CT_RUN(test_cache_is_all_or_nothing);
    CT_RUN(test_boot_cache_ignores_budget);
    CT_RUN(test_boot_cache_refused_far_over_budget);
    CT_RUN(test_stale_new_file_removed);
    CT_RUN(test_flash_reserve_and_overflow);
    CT_RUN(test_duplicate_top_level_sections);
    CT_RUN(test_xfer_dropped_on_close_and_idle);
    return CT_DONE();
}
