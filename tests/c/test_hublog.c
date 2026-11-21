#define _GNU_SOURCE
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

static void test_push_when_up(void) {
    static sink_t s;
    static hp_hublog_t l;
    memset(&s, 0, sizeof s);
    s.up = true;
    CT_EQ(hp_hublog_init(&l, emit, &s), 0);
    hp_hublog_write(&l, "hub: a\nhub: b fai", 17);
    hp_hublog_write(&l, "led\n\n", 5); /* a line split over two writes; an empty line is skipped */
    struct pollfd p = {hp_hublog_wake_fd(&l), POLLIN, 0};
    CT_EQ(poll(&p, 1, 0), 1); /* event-driven: the wake fd is readable, no timer involved */
    hp_hublog_service(&l);
    CT_EQ(s.n, 2);
    CT_CHECK(!strcmp(s.msg[0], "hub: a") && !strcmp(s.level[0], "info"));
    CT_CHECK(!strcmp(s.msg[1], "hub: b failed") && !strcmp(s.level[1], "warning"));
    CT_EQ(poll(&p, 1, 0), 0); /* drained */
}

static void test_buffer_while_down_then_flush(void) {
    static sink_t s;
    static hp_hublog_t l;
    memset(&s, 0, sizeof s);
    CT_EQ(hp_hublog_init(&l, emit, &s), 0);
    for (int i = 0; i < HP_LOG_RING + 6; i++) {
        char b[32];
        int n = snprintf(b, sizeof b, "line %d\n", i);
        hp_hublog_write(&l, b, (size_t)n);
        hp_hublog_service(&l); /* nobody connected: stays queued */
    }
    CT_EQ(s.n, 0);
    s.up = true;
    hp_hublog_service(&l); /* the hello just completed */
    CT_EQ(s.n, HP_LOG_RING);
    CT_CHECK(!strcmp(s.msg[0], "line 6")); /* the 6 oldest were dropped, bounded */
    CT_EQ(s.dropped[0], 6);
    CT_EQ(s.dropped[1], 0);
    CT_CHECK(!strcmp(s.msg[HP_LOG_RING - 1], "line 69"));
}

static void test_long_line_cut(void) {
    static sink_t s;
    static hp_hublog_t l;
    memset(&s, 0, sizeof s);
    s.up = true;
    CT_EQ(hp_hublog_init(&l, emit, &s), 0);
    char big[600];
    memset(big, 'x', sizeof big);
    big[599] = '\n';
    hp_hublog_write(&l, big, sizeof big);
    hp_hublog_write(&l, "next\n", 5);
    hp_hublog_service(&l);
    CT_EQ(s.n, 2);
    CT_EQ(strlen(s.msg[0]), HP_LOG_LINE - 1);
    CT_CHECK(!strcmp(s.msg[1], "next"));
}

static void test_capture_stderr(void) {
    static sink_t s;
    static hp_hublog_t l;
    memset(&s, 0, sizeof s);
    s.up = true;
    char path[] = "/tmp/hublogXXXXXX";
    int fd = mkstemp(path), saved = dup(STDERR_FILENO);
    CT_CHECK(fd >= 0 && saved >= 0);
    dup2(fd, STDERR_FILENO);
    CT_EQ(hp_hublog_init(&l, emit, &s), 0);
    CT_EQ(hp_hublog_capture(&l), 0);
    fprintf(stderr, "hub: captured %d\n", 7);
    struct pollfd p = {hp_hublog_wake_fd(&l), POLLIN, 0};
    CT_EQ(poll(&p, 1, 2000), 1); /* the reader thread woke us; well under a second */
    hp_hublog_service(&l);
    hp_hublog_release(&l);
    dup2(saved, STDERR_FILENO);
    CT_EQ(s.n, 1);
    CT_CHECK(!strcmp(s.msg[0], "hub: captured 7"));
    char buf[64] = "";
    CT_CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0);
    CT_CHECK(!strcmp(buf, "hub: captured 7\n")); /* still teed to the original stderr (the tmpfs log) */
    close(fd);
    close(saved);
    unlink(path);
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
    CT_RUN(test_buffer_while_down_then_flush);
    CT_RUN(test_long_line_cut);
    CT_RUN(test_capture_stderr);
    CT_RUN(test_emit_needs_a_client);
    CT_RUN(test_only_home_assistant_hellos_get_logs);
    return CT_DONE();
}
