#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/hub_handlers.h"
#include "hub/net/jsonrpc.h"

static hp_handler_tbl_t tbl = {.token = "secret", .hub_id = "hub-1", .fw_version = "9"};
static char rep[4096];
static size_t rl;

static int run(hp_session_t *s, const char *json) {
    memset(rep, 0, sizeof rep);
    return hp_dispatch(json, strlen(json), &tbl, s, rep, sizeof rep, &rl);
}

static bool has(const char *needle) { return strstr(rep, needle) != NULL; }

static int echo_handler(void *ctx, hp_session_t *s, uint32_t id, const cJSON *d, char *reply,
                        size_t cap, size_t *len) {
    (void)ctx; (void)s; (void)d;
    return hp_make_reply(id, "command_result", NULL, reply, cap, len);
}

static void test_errors(void) {
    hp_session_t s = {.authed = true};
    CT_EQ(run(&s, "{\"v\":1,\"id\":5,\"t\":\"nope\",\"d\":{}}"), HP_DISPATCH_OK);
    CT_CHECK(has("\"unknown_type\"") && has("\"id\":5") && has("\"t\":\"error\""));
    run(&s, "{\"v\":2,\"id\":7,\"t\":\"ping\",\"d\":{}}");
    CT_CHECK(has("\"version\"") && has("\"id\":7"));
    run(&s, "{\"v\":4294967295,\"id\":4294967295,\"t\":\"ping\",\"d\":{}}");
    CT_CHECK(has("\"version\"") && has("\"id\":4294967295"));
    run(&s, "{\"id\":7,\"t\":\"ping\",\"d\":{}}");
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":8,\"t\":\"set_config\",\"d\":{}}"); /* missing required */
    CT_CHECK(has("\"bad_request\"") && has("\"id\":8"));
    run(&s, "{\"v\":1,\"id\":9,\"t\":\"set_config\",\"d\":{\"config\":5}}"); /* json must be obj/arr */
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":9,\"t\":\"key_event\",\"d\":{\"remote\":256,\"key\":1,\"action\":0}}");
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":9,\"t\":\"key_event\",\"d\":{\"remote\":1,\"key\":1.5,\"action\":0}}");
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":9,\"t\":\"key_event\",\"d\":{\"remote\":1,\"key\":1,\"action\":7}}");
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":9,\"t\":\"ping\",\"d\":{\"x\":1}}"); /* unknown field */
    CT_CHECK(has("\"bad_request\""));
    run(&s, "{\"v\":1,\"id\":-1,\"t\":\"ping\"}");
    CT_CHECK(has("\"bad_request\"") && has("\"id\":0"));
    run(&s, "not json");
    CT_CHECK(has("\"bad_request\"") && has("\"id\":0"));
    run(&s, "[1]");
    CT_CHECK(has("\"bad_request\""));
    CT_EQ(hp_dispatch("{\"v\":1,\"id\":1,\"t\":\"ping\"}\xff", 27, &tbl, &s, rep, sizeof rep, &rl), 0);
    CT_CHECK(has("\"bad_request\""));
    /* truncated multi-byte char inside a string field */
    CT_EQ(hp_dispatch("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"\xc3\"}}", 44, &tbl, &s,
                      rep, sizeof rep, &rl), 0);
    CT_CHECK(has("\"bad_request\""));
    /* deeply nested input is rejected, not a stack overflow */
    char deep[2000];
    memset(deep, '[', sizeof deep - 1);
    deep[sizeof deep - 1] = 0;
    run(&s, deep);
    CT_CHECK(has("\"bad_request\""));
    /* reply buffer too small */
    char tiny[8];
    CT_EQ(hp_dispatch("{\"v\":1,\"id\":1,\"t\":\"nope\"}", 25, &tbl, &s, tiny, sizeof tiny, &rl), -HP_E_TOOBIG);
}

static void test_auth(void) {
    hp_session_t s = {0};
    CT_EQ(run(&s, "{\"v\":1,\"id\":3,\"t\":\"ping\",\"d\":{}}"), HP_DISPATCH_CLOSE);
    CT_CHECK(has("\"unauthorized\"") && has("\"id\":3") && !s.authed);
    CT_EQ(run(&s, "{\"v\":1,\"id\":4,\"t\":\"hello\",\"d\":{\"token\":\"wrong\"}}"), HP_DISPATCH_CLOSE);
    CT_CHECK(has("\"unauthorized\"") && !s.authed);
    CT_EQ(run(&s, "{\"v\":1,\"id\":4,\"t\":\"hello\",\"d\":{\"token\":\"secre\"}}"), HP_DISPATCH_CLOSE);
    CT_EQ(run(&s, "{\"v\":1,\"id\":4,\"t\":\"hello\",\"d\":{\"token\":\"secret!\"}}"), HP_DISPATCH_CLOSE);
    CT_EQ(run(&s, "{\"v\":1,\"id\":4,\"t\":\"hello\",\"d\":{}}"), HP_DISPATCH_OK);
    CT_CHECK(has("\"bad_request\"") && !s.authed);
    CT_EQ(run(&s, "{\"v\":1,\"id\":10,\"t\":\"hello\",\"d\":{\"token\":\"secret\",\"client\":\"ha\"}}"), HP_DISPATCH_OK);
    CT_CHECK(has("\"hello_ack\"") && has("\"hub_id\":\"hub-1\"") && has("\"id\":10") && s.authed);
    CT_EQ(run(&s, "{\"v\":1,\"id\":11,\"t\":\"ping\",\"d\":{}}"), HP_DISPATCH_OK);
    CT_CHECK(has("\"pong\"") && has("\"id\":11"));
    /* no configured token: nothing authenticates */
    hp_handler_tbl_t none = {0};
    hp_session_t s2 = {0};
    hp_dispatch("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"\"}}", 44, &none, &s2, rep, sizeof rep, &rl);
    CT_CHECK(has("\"unauthorized\"") && !s2.authed);
}

static void test_handler(void) {
    hp_session_t s = {.authed = true};
    tbl.handler[HP_MSG_COMMAND] = echo_handler;
    run(&s, "{\"v\":1,\"id\":12,\"t\":\"command\",\"d\":{\"device\":\"tv\",\"command\":\"on\"}}");
    CT_CHECK(has("\"command_result\"") && has("\"id\":12"));
    tbl.handler[HP_MSG_COMMAND] = NULL;
}

static void test_broadcast_without_server_frees(void) {
    hub_ctx_t h;
    memset(&h, 0, sizeof h); /* srv == NULL: LSan flags a leaked d */
    hub_broadcast(&h, "state_update", cJSON_CreateObject());
}

int main(void) {
    CT_RUN(test_errors);
    CT_RUN(test_auth);
    CT_RUN(test_handler);
    CT_RUN(test_broadcast_without_server_frees);
    return CT_DONE();
}
