#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hub_handlers.h"
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

int main(void) {
    CT_RUN(test_push_when_up);
    CT_RUN(test_line_reaches_the_file_synchronously);
    CT_RUN(test_stalled_ring_never_blocks);
    CT_RUN(test_rate_limit_and_summary);
    CT_RUN(test_long_line_cut_in_the_ring_not_the_file);
    CT_RUN(test_child_pipe_is_nonblocking);
    CT_RUN(test_emit_needs_a_client);
    CT_RUN(test_only_home_assistant_hellos_get_logs);
    return CT_DONE();
}
