#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hub_handlers.h"
#include "hub/net/hooklog.h"
#include "hub/net/hublog.h"

typedef struct {
    bool up; /* a client is authenticated */
    int n;
    char msg[200][HP_LOG_LINE];
    char level[200][8];
    uint32_t dropped[200];
} sink_t;

static bool emit(void *ctx, const hp_logline_t *l) {
    sink_t *s = ctx;
    if (!s->up) return false;
    snprintf(s->msg[s->n], HP_LOG_LINE, "%s", l->msg);
    snprintf(s->level[s->n], 8, "%s", l->level);
    s->dropped[s->n++] = l->dropped;
    return true;
}

static uint32_t g_ms;
static uint32_t fake_now(void) { return g_ms; }

/* A hublog with its own out file (not stderr), the fake clock, installed for hp_log. */
static int setup(hp_hublog_t *l, sink_t *s, char *path) {
    memset(s, 0, sizeof *s);
    int fd = mkstemp(path);
    CT_CHECK(fd >= 0);
    CT_EQ(hp_hublog_init(l, emit, s), 0);
    l->out_fd = fd;
    l->now_ms = fake_now;
    l->last_ms = g_ms;
    hp_hublog_install(l);
    return fd;
}

static void teardown(int fd, const char *path) {
    hp_hublog_install(NULL);
    close(fd);
    unlink(path);
}

static void test_push_when_up(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    s.up = true;
    hp_log("hub: a\n");
    hp_log("hub: b %s", "failed"); /* newline added */
    struct pollfd p = {hp_hublog_wake_fd(&l), POLLIN, 0};
    CT_EQ(poll(&p, 1, 0), 1); /* event-driven: the wake fd is readable, no timer involved */
    hp_hublog_service(&l);
    CT_EQ(s.n, 2);
    CT_CHECK(!strcmp(s.msg[0], "hub: a") && !strcmp(s.level[0], "info"));
    CT_CHECK(!strcmp(s.msg[1], "hub: b failed") && !strcmp(s.level[1], "warning"));
    CT_EQ(poll(&p, 1, 0), 0); /* drained */
    teardown(fd, path);
}

/* Crash safety: the line is in the file the moment hp_log returns; nothing needs the loop, a thread or the ring. */
static void test_line_reaches_the_file_synchronously(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    hp_log("hub: before the crash %d", 1);
    char buf[64] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0);
    CT_CHECK(!strcmp(buf, "hub: before the crash 1\n"));
    CT_EQ(s.n, 0); /* never serviced */
    teardown(fd, path);
}

static void test_stalled_ring_never_blocks(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    for (int i = 0; i < 500; i++) { /* nobody takes lines; the clock moves so the rate limit does not hide the ring */
        g_ms += 200;
        hp_log("hub: line %d", i);
    }
    s.up = true;
    hp_hublog_service(&l);
    CT_EQ(s.n, HP_LOG_RING); /* bounded */
    CT_CHECK(!strcmp(s.msg[HP_LOG_RING - 1], "hub: line 499"));
    CT_EQ(s.dropped[0], 500 - HP_LOG_RING); /* the overflow is counted */
    /* lock contention: the ring is busy, the line is dropped from it (counted) but still written */
    pthread_mutex_lock(&l.mu);
    g_ms += 200;
    hp_log("hub: while busy");
    pthread_mutex_unlock(&l.mu);
    CT_EQ(atomic_load(&l.lost), 1);
    char buf[8192] = "";
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    CT_CHECK(n > 0 && strstr(buf, "hub: while busy\n") && strstr(buf, "hub: line 0\n")); /* the file has everything */
    teardown(fd, path);
}

static void test_rate_limit_and_summary(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    g_ms = 1000;
    int fd = setup(&l, &s, path);
    s.up = true;
    for (int i = 0; i < 40; i++) hp_log("hub: burst %d", i); /* same millisecond */
    hp_hublog_service(&l);
    CT_EQ(s.n, HP_LOG_BURST); /* the bucket's burst; 10 suppressed */
    char buf[8192] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0 && strstr(buf, "hub: burst 39\n")); /* the file keeps them all */
    g_ms += 1000; /* HP_LOG_RATE tokens back */
    hp_log("hub: after");
    hp_hublog_service(&l);
    CT_EQ(s.n, HP_LOG_BURST + 2);
    CT_CHECK(!strcmp(s.msg[HP_LOG_BURST], "hub: 10 log lines suppressed"));
    CT_CHECK(!strcmp(s.msg[HP_LOG_BURST + 1], "hub: after"));
    teardown(fd, path);
}

static void test_long_line_cut_in_the_ring_not_the_file(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    s.up = true;
    char big[301];
    memset(big, 'x', 300);
    big[300] = 0;
    hp_log("%s", big);
    hp_hublog_service(&l);
    CT_EQ(s.n, 1);
    CT_EQ(strlen(s.msg[0]), HP_LOG_LINE - 1);
    char buf[400] = "";
    CT_EQ(pread(fd, buf, sizeof buf - 1, 0), 301);
    teardown(fd, path);
}

/* The bt helper's stderr pipe: O_NONBLOCK, a stalled reader drops bytes and never blocks the writer. */
static void test_child_pipe_is_nonblocking(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    s.up = true;
    int ep[2];
    CT_EQ(pipe2(ep, O_CLOEXEC | O_NONBLOCK), 0);
    CT_CHECK(fcntl(ep[1], F_GETFL) & O_NONBLOCK);
    long wrote = 0;
    for (;;) { /* nobody reads: the pipe fills and the write fails with EAGAIN instead of blocking */
        ssize_t w = write(ep[1], "bt: spam spam spam spam\n", 24);
        if (w < 0) { CT_EQ(errno, EAGAIN); break; }
        wrote += w;
        CT_CHECK(wrote < (1 << 22));
    }
    hp_linebuf_t lb;
    memset(&lb, 0, sizeof lb);
    char b[256];
    ssize_t n = read(ep[0], b, sizeof b);
    CT_CHECK(n > 0);
    hp_hublog_feed(&l, &lb, b, (size_t)n); /* line-split, to the file and the ring; a cut last line stays pending */
    hp_hublog_service(&l);
    CT_CHECK(s.n >= 1 && !strncmp(s.msg[0], "bt: spam", 8));
    char buf[64] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0 && !strncmp(buf, "bt: spam", 8));
    close(ep[0]);
    close(ep[1]);
    teardown(fd, path);
}

static void test_emit_needs_a_client(void) {
    static hub_ctx_t h;
    hub_ctx_init(&h);
    hp_logline_t ln = {"info", "x", 1, 0};
    CT_CHECK(!hub_log_emit(&h, &ln)); /* no server, no client: keep the line queued */
}

static void test_only_home_assistant_hellos_get_logs(void) {
    hp_handler_tbl_t tbl;
    memset(&tbl, 0, sizeof tbl);
    tbl.token = "tok";
    tbl.hub_id = "h";
    hp_session_t sess;
    char reply[2048];
    size_t rl = 0;
    const char *tool = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\",\"client\":\"deploy\"}}";
    const char *ha = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\",\"client\":\"home-assistant\"}}";
    const char *bare = "{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}";
    memset(&sess, 0, sizeof sess);
    CT_EQ(hp_dispatch(ha, strlen(ha), &tbl, &sess, reply, sizeof reply, &rl), 0);
    CT_CHECK(sess.authed && sess.log_sink);
    CT_EQ(hp_dispatch(tool, strlen(tool), &tbl, &sess, reply, sizeof reply, &rl), 0);
    CT_CHECK(sess.authed && !sess.log_sink); /* a new hello resets it */
    CT_EQ(hp_dispatch(bare, strlen(bare), &tbl, &sess, reply, sizeof reply, &rl), 0);
    CT_CHECK(!sess.log_sink);
}

static void test_hook_log_is_shipped_once_then_followed(void) {
    static sink_t s;
    static hp_hublog_t l;
    static hp_hooklog_t h;
    char path[] = "/tmp/hublogXXXXXX", hook[] = "/tmp/hubhookXXXXXX.log";
    int fd = setup(&l, &s, path);
    s.up = true;
    int hf = mkstemps(hook, 4);
    CT_CHECK(hf >= 0);
    CT_EQ(write(hf, "[t] ha-harmony: starting\n[t] ha-harmony: respawn 1 of 3 in 5s\n", 62), 62);
    hp_hooklog_start(&h, &l, hook); /* the file as the daemon finds it at start */
    hp_hublog_service(&l);
    CT_EQ(s.n, 2);
    CT_CHECK(!strcmp(s.msg[0], "[boot-hook] [t] ha-harmony: starting"));
    CT_CHECK(!strcmp(s.msg[1], "[boot-hook] [t] ha-harmony: respawn 1 of 3 in 5s"));
    CT_EQ(lseek(fd, 0, SEEK_END), 0); /* shipped only: not copied into the daemon log */
    /* a later hook write wakes the poll loop through inotify (no timer) and ships just the new line */
    CT_CHECK(hp_hooklog_fd(&h) >= 0);
    CT_EQ(write(hf, "[t] ha-harmony: healthy after 120s\n", 36), 36);
    struct pollfd p = {hp_hooklog_fd(&h), POLLIN, 0};
    CT_EQ(poll(&p, 1, 2000), 1);
    hp_hooklog_service(&h);
    hp_hublog_service(&l);
    CT_EQ(s.n, 3);
    CT_CHECK(!strcmp(s.msg[2], "[boot-hook] [t] ha-harmony: healthy after 120s"));
    CT_EQ(ftruncate(hf, 0), 0); /* a new boot: restarts from the top */
    CT_EQ(pwrite(hf, "[t] new\n", 8, 0), 8);
    hp_hooklog_service(&h);
    hp_hublog_service(&l);
    CT_EQ(s.n, 4);
    CT_CHECK(!strcmp(s.msg[3], "[boot-hook] [t] new"));
    char out[64];
    CT_CHECK(hp_hooklog_path_beside("/var/volatile/ha-harmony.healthy", out, sizeof out) &&
             !strcmp(out, "/var/volatile/ha-harmony-hook.log"));
    close(hf);
    unlink(hook);
    unlink(h.offpath);
    teardown(fd, path);
}

/* A respawned daemon (a fresh hp_hooklog_t) continues from the offset kept in tmpfs; a truncated log restarts at 0. */
static void test_hook_offset_survives_a_respawn(void) {
    static sink_t s;
    static hp_hublog_t l;
    static hp_hooklog_t h1, h2, h3;
    char path[] = "/tmp/hublogXXXXXX", hook[] = "/tmp/hubhookXXXXXX.log";
    int fd = setup(&l, &s, path);
    s.up = true;
    int hf = mkstemps(hook, 4);
    CT_CHECK(hf >= 0);
    CT_EQ(write(hf, "one\ntwo\n", 8), 8);
    hp_hooklog_start(&h1, &l, hook);
    hp_hublog_service(&l);
    CT_EQ(s.n, 2);
    CT_CHECK(strlen(h1.offpath) > 0 && access(h1.offpath, R_OK) == 0); /* the offset file is beside the log, on tmpfs */
    CT_EQ(write(hf, "three\n", 6), 6); /* written while the daemon is down (a respawn note) */
    hp_hooklog_start(&h2, &l, hook);    /* the new daemon */
    hp_hublog_service(&l);
    CT_EQ(s.n, 3); /* only the new line */
    CT_CHECK(!strcmp(s.msg[2], "[boot-hook] three"));
    CT_EQ(ftruncate(hf, 0), 0); /* a new boot truncates the log: the stored offset is stale */
    CT_EQ(pwrite(hf, "x\n", 2, 0), 2);
    hp_hooklog_start(&h3, &l, hook);
    hp_hublog_service(&l);
    CT_EQ(s.n, 4);
    CT_CHECK(!strcmp(s.msg[3], "[boot-hook] x"));
    close(hf);
    unlink(hook);
    unlink(h1.offpath);
    teardown(fd, path);
}

static int rf_send(void *self, const uint8_t *f, size_t n) { (void)self; (void)f; (void)n; return 0; }
static int rf_recv(void *self, uint8_t *f, size_t cap, int ms) { (void)self; (void)f; (void)cap; (void)ms; return 0; }
static int rf_claim(void *self) { (void)self; return 0; }

/* --rf-debug link lines go through hp_log (so they ship), not straight to stderr. */
static void test_rf_link_log_goes_through_hp_log(void) {
    static sink_t s;
    static hp_hublog_t l;
    static hub_ctx_t h;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    s.up = true;
    hub_ctx_init(&h);
    hp_rf_transport tp = {NULL, rf_send, rf_recv, rf_claim, NULL, NULL, NULL};
    h.sinks.rf = &tp;
    h.rf_debug = true;
    hub_pump_rf(&h); /* binds the link */
    CT_CHECK(h.link.log != NULL);
    h.link.log(h.link.log_ctx, "link 1 ms: up");
    hp_hublog_service(&l);
    CT_EQ(s.n, 1);
    CT_CHECK(!strcmp(s.msg[0], "link 1 ms: up"));
    char buf[64] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0 && !strcmp(buf, "link 1 ms: up\n"));
    teardown(fd, path);
}

/* A shipped line carries "[HH:MM:SS.mmm +<uptime>ms] "; the file keeps the bare line. */
static void test_shipped_lines_are_stamped(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    s.up = true;
    l.stamp = true;
    g_ms += 1234;
    hp_log("hub: stamped");
    hp_hublog_service(&l);
    CT_EQ(s.n, 1);
    unsigned hh, mm, ss, ms, up;
    char rest[64];
    CT_EQ(sscanf(s.msg[0], "[%2u:%2u:%2u.%3u +%ums] %63[^\n]", &hh, &mm, &ss, &ms, &up, rest), 6);
    CT_EQ(up, g_ms);
    CT_CHECK(!strcmp(rest, "hub: stamped"));
    char buf[64] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0 && !strcmp(buf, "hub: stamped\n"));
    teardown(fd, path);
}

/* The watchdog: > HP_STALL_MS logs "loop stall <ms> ms in <handler>"; at the threshold or below, nothing. */
static void test_stall_watchdog_threshold(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = setup(&l, &s, path);
    CT_CHECK(!hp_stall_check("fast", 1000, 1000 + HP_STALL_MS));
    CT_CHECK(hp_stall_check("rf_spi write", 1000, 1000 + HP_STALL_MS + 1));
    CT_CHECK(hp_stall_check("wrap", 0xFFFFFF00u, 0x00000200u)); /* 32-bit wrap: 768 ms */
    uint32_t t0 = hp_mono_ms();
    CT_CHECK(!hp_stall_end("now", t0));
    char buf[256] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0);
    CT_CHECK(!strcmp(buf, "hub: loop stall 251 ms in rf_spi write\nhub: loop stall 768 ms in wrap\n"));
    /* the poll's own overrun only: time inside ws callbacks and the requested wait are not "poll wake" */
    CT_CHECK(!hp_stall_poll(5000, 5000 + 1000 + 900, 1000, 900)); /* slow callback, on-time poll */
    CT_CHECK(!hp_stall_poll(5000, 5000 + 4000, -1, 4000));        /* woken, all spent in callbacks */
    CT_CHECK(!hp_stall_poll(5000, 5000 + 100, 1000, 0));          /* early wake: nothing */
    CT_CHECK(!hp_stall_poll(5000, 5000 + 150 + 40, 1000, 40));    /* early wake with callback time */
    CT_CHECK(!hp_stall_poll(0xFFFFFF00u, 0x00000040u, 1000, 0));  /* early wake across the 32-bit wrap */
    CT_CHECK(hp_stall_poll(0xFFFFFF00u, 1044u, 1000, 0));         /* 300 late across the wrap */
    CT_CHECK(!hp_stall_check("clock back", 5000, 4990));          /* now before t0: no stall */
    CT_CHECK(hp_stall_poll(5000, 5000 + 1000 + 900 + 300, 1000, 900)); /* 300 ms late beyond both */
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0);
    CT_CHECK(strstr(buf, "hub: loop stall 300 ms in poll wake\n"));
    CT_CHECK(!strcmp(hp_hublog_level("hub: loop stall 251 ms in x"), "warning"));
    teardown(fd, path);
}

static void test_warnings_have_their_own_bucket(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    g_ms = 1000;
    int fd = setup(&l, &s, path);
    s.up = true;
    for (int i = 0; i < 100; i++) hp_log("hub: chatter %d", i); /* exhausts the info bucket */
    hp_log("hub: loop stall 300 ms in x");
    hp_hublog_service(&l);
    CT_CHECK(!strcmp(s.msg[s.n - 1], "hub: loop stall 300 ms in x"));
    CT_CHECK(!strcmp(s.level[s.n - 1], "warning"));
    teardown(fd, path);
}

static void test_overflow_drops_info_before_warnings(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    g_ms = 1000;
    int fd = setup(&l, &s, path); /* nobody up: lines stay queued */
    hp_log("hub: first failed");
    for (int i = 0; i < HP_LOG_RING + 10; i++) {
        hp_log("hub: info %d", i);
        g_ms += 100; /* keep the bucket full */
    }
    hp_log("hub: second failed");
    s.up = true;
    hp_hublog_service(&l);
    CT_EQ(s.n, HP_LOG_RING);
    CT_CHECK(!strcmp(s.msg[0], "hub: first failed")); /* the oldest line survived: it is a warning */
    CT_CHECK(!strcmp(s.msg[s.n - 1], "hub: second failed"));
    teardown(fd, path);
}

static void test_readable_only_after_a_push(void) {
    static sink_t s;
    static hp_hublog_t l;
    char path[] = "/tmp/hublogXXXXXX";
    g_ms = 1000;
    int fd = setup(&l, &s, path);
    s.up = true;
    CT_CHECK(!hp_hublog_readable(&l));
    hp_log("hub: x");
    CT_CHECK(hp_hublog_readable(&l));
    hp_hublog_service(&l);
    CT_CHECK(!hp_hublog_readable(&l));
    teardown(fd, path);
}

int main(void) {
    CT_RUN(test_hook_offset_survives_a_respawn);
    CT_RUN(test_rf_link_log_goes_through_hp_log);
    CT_RUN(test_hook_log_is_shipped_once_then_followed);
    CT_RUN(test_push_when_up);
    CT_RUN(test_shipped_lines_are_stamped);
    CT_RUN(test_stall_watchdog_threshold);
    CT_RUN(test_line_reaches_the_file_synchronously);
    CT_RUN(test_stalled_ring_never_blocks);
    CT_RUN(test_rate_limit_and_summary);
    CT_RUN(test_warnings_have_their_own_bucket);
    CT_RUN(test_overflow_drops_info_before_warnings);
    CT_RUN(test_readable_only_after_a_push);
    CT_RUN(test_long_line_cut_in_the_ring_not_the_file);
    CT_RUN(test_child_pipe_is_nonblocking);
    CT_RUN(test_emit_needs_a_client);
    CT_RUN(test_only_home_assistant_hellos_get_logs);
    return CT_DONE();
}
