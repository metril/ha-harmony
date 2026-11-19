#define _DEFAULT_SOURCE
/* Hub identity: id/name validation, persistence, precedence (flag > file > default), the pinned refusal,
 * set_hub_id through the dispatcher, hello_ack fields, the stable hardware id, mDNS TXT hw= and rename. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hub_handlers.h"
#include "hub/net/ident.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/mdns.h"
#include "hub/net/mdns_srv.h"

static char root[] = "/tmp/hub_ident_test_XXXXXX";
static int sub_n;

static char *newdir(void) {
    static char d[300];
    snprintf(d, sizeof d, "%s/c%d", root, sub_n++);
    CT_EQ(mkdir(d, 0755), 0);
    return d;
}

static void put(const char *dir, const char *file, const char *text) {
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "wb");
    CT_CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool slurp(const char *dir, const char *file, char *out, size_t cap) {
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    return true;
}

static void test_validation(void) {
    CT_CHECK(hp_ident_id_valid("living-room"));
    CT_CHECK(hp_ident_id_valid("A_b-c9"));
    CT_CHECK(!hp_ident_id_valid("a.b")); /* new ids: no dot (not an mDNS label) */
    CT_CHECK(hp_ident_id_existing_ok("A.b_c-9") && !hp_ident_id_existing_ok("default"));
    char lab[HP_ID_MAX + 1];
    hp_ident_mdns_label("a.b.c", lab);
    CT_CHECK(!strcmp(lab, "a-b-c"));
    CT_CHECK(hp_ident_id_valid("a"));
    char l63[64], l64[65];
    memset(l63, 'a', 63);
    l63[63] = 0;
    memset(l64, 'a', 64);
    l64[64] = 0;
    CT_CHECK(hp_ident_id_valid(l63));
    CT_CHECK(!hp_ident_id_valid(l64));
    CT_CHECK(!hp_ident_id_valid(""));
    CT_CHECK(!hp_ident_id_valid("default"));
    CT_CHECK(hp_ident_id_valid("Default2"));
    CT_CHECK(!hp_ident_id_valid("has space"));
    CT_CHECK(!hp_ident_id_valid("a/b"));
    CT_CHECK(!hp_ident_id_valid("caf\xc3\xa9"));
    CT_CHECK(!hp_ident_id_valid("a\n"));
    CT_CHECK(!hp_ident_id_valid(NULL));
    CT_CHECK(hp_ident_name_valid(""));
    CT_CHECK(hp_ident_name_valid("Living room \xc3\xa9"));
    char n48[49], n49[50];
    memset(n48, 'x', 48);
    n48[48] = 0;
    memset(n49, 'x', 49);
    n49[49] = 0;
    CT_CHECK(hp_ident_name_valid(n48));
    CT_CHECK(!hp_ident_name_valid(n49));
    CT_CHECK(!hp_ident_name_valid("bad\xff"));
    CT_CHECK(!hp_ident_name_valid("tab\there"));
    CT_CHECK(!hp_ident_name_valid("nl\n"));
    CT_CHECK(!hp_ident_name_valid("del\x7f"));
}

static void test_precedence_and_persistence(void) {
    hp_ident_t t;
    char *d = newdir();
    /* nothing persisted: the default */
    CT_EQ(hp_ident_init(&t, d, NULL, "ha-harmony-host", "aabbccddeeff"), 0);
    CT_CHECK(!strcmp(t.id, "ha-harmony-host") && !t.pinned && !t.name[0] && !strcmp(t.hw, "aabbccddeeff"));
    /* set persists, atomically, in the state dir only */
    CT_EQ(hp_ident_set(&t, "living-room", "Living room"), HP_IDENT_OK);
    CT_CHECK(!strcmp(t.id, "living-room") && !strcmp(t.name, "Living room"));
    char b[100];
    CT_CHECK(slurp(d, "hub_id", b, sizeof b) && !strcmp(b, "living-room"));
    CT_CHECK(slurp(d, "hub_name", b, sizeof b) && !strcmp(b, "Living room"));
    /* a restart: the file beats the default */
    CT_EQ(hp_ident_init(&t, d, NULL, "ha-harmony-host", NULL), 0);
    CT_CHECK(!strcmp(t.id, "living-room") && !strcmp(t.name, "Living room") && !t.pinned);
    /* the --hub-id flag beats the file; the name still comes from its file */
    CT_EQ(hp_ident_init(&t, d, "pinned-id", "ha-harmony-host", NULL), 0);
    CT_CHECK(!strcmp(t.id, "pinned-id") && t.pinned && !strcmp(t.name, "Living room"));
    /* pinned: a different id is refused and nothing changes; the same id and the name are fine */
    CT_EQ(hp_ident_set(&t, "other", NULL), HP_IDENT_PINNED);
    CT_CHECK(!strcmp(t.id, "pinned-id"));
    CT_CHECK(slurp(d, "hub_id", b, sizeof b) && !strcmp(b, "living-room")); /* the file is untouched */
    CT_EQ(hp_ident_set(&t, "pinned-id", "Den"), HP_IDENT_OK);
    CT_CHECK(!strcmp(t.name, "Den"));
    /* name omitted keeps it, "" clears it (file removed) */
    CT_EQ(hp_ident_init(&t, d, NULL, "x", NULL), 0);
    CT_EQ(hp_ident_set(&t, "kitchen", NULL), HP_IDENT_OK);
    CT_CHECK(!strcmp(t.name, "Den"));
    CT_EQ(hp_ident_set(&t, "kitchen", ""), HP_IDENT_OK);
    CT_CHECK(!t.name[0]);
    CT_CHECK(!slurp(d, "hub_name", b, sizeof b));
    /* invalid input changes nothing */
    CT_EQ(hp_ident_set(&t, "default", "n"), HP_IDENT_BAD_ID);
    CT_EQ(hp_ident_set(&t, "bad id", NULL), HP_IDENT_BAD_ID);
    CT_EQ(hp_ident_set(&t, "-lead", NULL), HP_IDENT_BAD_ID);
    CT_EQ(hp_ident_set(&t, ".lead", NULL), HP_IDENT_BAD_ID);
    CT_EQ(hp_ident_set(&t, "ok", "bad\xff"), HP_IDENT_BAD_NAME);
    CT_CHECK(!strcmp(t.id, "kitchen"));
    CT_CHECK(slurp(d, "hub_id", b, sizeof b) && !strcmp(b, "kitchen"));
    /* a corrupt or invalid file is ignored */
    char *d2 = newdir();
    put(d2, "hub_id", "not valid!\n");
    put(d2, "hub_name", "bad\xff");
    CT_EQ(hp_ident_init(&t, d2, NULL, "ha-harmony-host", NULL), 0);
    CT_CHECK(!strcmp(t.id, "ha-harmony-host") && !t.name[0]);
    put(d2, "hub_id", "with-newline\n");
    CT_EQ(hp_ident_init(&t, d2, NULL, "ha-harmony-host", NULL), 0);
    CT_CHECK(!strcmp(t.id, "with-newline"));
    /* the daemon never fails on the id: a bad pinned id is ignored (unpinned, default used), a bad default sanitised */
    CT_EQ(hp_ident_init(&t, d2, "default", "x", NULL), 0);
    CT_CHECK(!t.pinned && !strcmp(t.id, "with-newline") && t.warn[0]);
    CT_EQ(hp_ident_init(&t, NULL, "bad id", "x", NULL), 0);
    CT_CHECK(!t.pinned && !strcmp(t.id, "x") && t.warn[0]);
    char l64[80];
    memset(l64, 'b', 64);
    l64[64] = 0;
    CT_EQ(hp_ident_init(&t, NULL, l64, "x", NULL), 0);
    CT_CHECK(!t.pinned && !strcmp(t.id, "x") && t.warn[0]);
    CT_EQ(hp_ident_init(&t, NULL, "dotted.id", "x", NULL), 0); /* existing dotted pinned ids keep working */
    CT_CHECK(t.pinned && !strcmp(t.id, "dotted.id") && !t.warn[0]);
    char host[120] = "ha-harmony-";
    memset(host + 11, 'h', 70);
    host[81] = 0;
    CT_EQ(hp_ident_init(&t, NULL, NULL, host, NULL), 0); /* overlong derived id: truncated to a valid id */
    CT_CHECK(strlen(t.id) == 63 && hp_ident_id_valid(t.id) && t.warn[0]);
    CT_EQ(hp_ident_init(&t, NULL, NULL, "my host", NULL), 0);
    CT_CHECK(!strcmp(t.id, "my-host") && hp_ident_id_valid(t.id));
    CT_EQ(hp_ident_init(&t, NULL, NULL, "default", NULL), 0);
    CT_CHECK(hp_ident_id_valid(t.id) && strcmp(t.id, "default"));
    CT_EQ(hp_ident_init(&t, NULL, NULL, NULL, NULL), 0);
    CT_CHECK(hp_ident_id_valid(t.id));
    /* no directory: the change lives in memory */
    CT_EQ(hp_ident_init(&t, NULL, NULL, "mem", NULL), 0);
    CT_EQ(hp_ident_set(&t, "mem2", "N"), HP_IDENT_OK);
    CT_CHECK(!strcmp(t.id, "mem2"));
}

static void test_io_failure_changes_nothing(void) {
    hp_ident_t t;
    char *d = newdir();
    CT_EQ(hp_ident_init(&t, d, NULL, "first", NULL), 0);
    CT_EQ(hp_ident_set(&t, "second", "Name"), HP_IDENT_OK);
    /* the state dir disappears: the write fails, the live values stay */
    char p[400];
    snprintf(p, sizeof p, "%s/hub_id", d);
    unlink(p);
    snprintf(p, sizeof p, "%s/hub_name", d);
    unlink(p);
    CT_EQ(rmdir(d), 0);
    CT_EQ(hp_ident_set(&t, "third", "Other"), HP_IDENT_IO);
    CT_CHECK(!strcmp(t.id, "second") && !strcmp(t.name, "Name"));
}

static void test_hw_read(void) {
    char out[HP_HW_MAX + 1];
    char *d = newdir();
    hp_ident_hw_read(d, out);
    CT_CHECK(!out[0]);
    char eth[400], wlan[400];
    snprintf(eth, sizeof eth, "%s/eth0", d);
    snprintf(wlan, sizeof wlan, "%s/wlan0", d);
    CT_EQ(mkdir(eth, 0755), 0);
    CT_EQ(mkdir(wlan, 0755), 0);
    put(eth, "address", "00:00:00:00:00:00\n"); /* all-zero: skipped */
    hp_ident_hw_read(d, out);
    CT_CHECK(!out[0]);
    put(eth, "address", "AA:bb:CC:dd:EE:01\n");
    hp_ident_hw_read(d, out);
    CT_CHECK(!strcmp(out, "aabbccddee01"));
    put(wlan, "address", "11:22:33:44:55:66\n"); /* wlan0 comes first */
    hp_ident_hw_read(d, out);
    CT_CHECK(!strcmp(out, "112233445566"));
    put(wlan, "address", "garbage\n"); /* unusable: falls through to eth0 */
    hp_ident_hw_read(d, out);
    CT_CHECK(!strcmp(out, "aabbccddee01"));
}

/* ---- through the dispatcher ---- */
static hub_ctx_t H;
static hp_handler_tbl_t T;
static hp_session_t S;
static char reply[4096];
static int changed_calls;
static char changed_to[80];

static void on_changed(void *ctx, const hp_ident_t *id) {
    (void)ctx;
    changed_calls++;
    snprintf(changed_to, sizeof changed_to, "%s", id->id);
}

static void setup(const char *dir, const char *flag) {
    memset(&S, 0, sizeof S);
    memset(&T, 0, sizeof T);
    hub_ctx_init(&H);
    hub_install(&T, &H);
    T.token = "tok";
    CT_EQ(hp_ident_init(&H.ident, dir, flag, "ha-harmony-host", "aabbccddeeff"), 0);
    H.ident_changed = on_changed;
    T.hub_id = H.ident.id;
    changed_calls = 0;
}

static void send_msg(const char *json) {
    size_t n = 0;
    reply[0] = 0;
    CT_CHECK(hp_dispatch(json, strlen(json), &T, &S, reply, sizeof reply, &n) >= 0);
    reply[n] = 0;
}
static bool has(const char *needle) { return strstr(reply, needle) != NULL; }

static void test_dispatch(void) {
    char *d = newdir();
    setup(d, NULL);
    send_msg("{\"v\":1,\"id\":1,\"t\":\"set_hub_id\",\"d\":{\"id\":\"x\"}}");
    CT_CHECK(has("unauthorized"));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}");
    CT_CHECK(has("\"hub_id\":\"ha-harmony-host\"") && has("\"hw_id\":\"aabbccddeeff\"") && has("\"rename\":true") &&
             !has("\"name\"") && !has("id_pinned"));
    send_msg("{\"v\":1,\"id\":3,\"t\":\"set_hub_id\",\"d\":{\"id\":\"living-room\",\"name\":\"Living room\"}}");
    CT_CHECK(has("\"t\":\"hub_id_ack\"") && has("\"id\":3") && has("\"hub_id\":\"living-room\"") && has("\"name\":\"Living room\""));
    CT_EQ(changed_calls, 1);
    CT_CHECK(!strcmp(changed_to, "living-room"));
    /* applied at once for hello_ack */
    send_msg("{\"v\":1,\"id\":4,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}");
    CT_CHECK(has("\"hub_id\":\"living-room\"") && has("\"name\":\"Living room\""));
    /* same id, new name: no re-announce */
    send_msg("{\"v\":1,\"id\":5,\"t\":\"set_hub_id\",\"d\":{\"id\":\"living-room\",\"name\":\"Den\"}}");
    CT_CHECK(has("hub_id_ack") && has("\"name\":\"Den\""));
    CT_EQ(changed_calls, 1);
    /* errors, nothing changes */
    send_msg("{\"v\":1,\"id\":6,\"t\":\"set_hub_id\",\"d\":{\"id\":\"default\"}}");
    CT_CHECK(has("\"code\":\"bad_id\"") && has("\"id\":6"));
    send_msg("{\"v\":1,\"id\":7,\"t\":\"set_hub_id\",\"d\":{\"id\":\"ok\",\"name\":\"\\u0007\"}}");
    CT_CHECK(has("\"code\":\"bad_name\""));
    send_msg("{\"v\":1,\"id\":8,\"t\":\"set_hub_id\",\"d\":{}}");
    CT_CHECK(has("bad_request"));
    send_msg("{\"v\":1,\"id\":9,\"t\":\"set_hub_id\",\"d\":{\"id\":5}}");
    CT_CHECK(has("bad_request"));
    CT_CHECK(!strcmp(H.ident.id, "living-room") && !strcmp(H.ident.name, "Den"));
    /* survives a restart */
    setup(d, NULL);
    CT_CHECK(!strcmp(H.ident.id, "living-room") && !strcmp(H.ident.name, "Den"));
}

static void test_dispatch_pinned(void) {
    char *d = newdir();
    setup(d, "pinned-id");
    send_msg("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}");
    CT_CHECK(has("\"hub_id\":\"pinned-id\"") && has("\"id_pinned\":true") && has("\"rename\":true"));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"set_hub_id\",\"d\":{\"id\":\"other\"}}");
    CT_CHECK(has("\"code\":\"id_pinned\"") && has("hub.conf"));
    CT_EQ(changed_calls, 0);
    CT_CHECK(!strcmp(H.ident.id, "pinned-id"));
    char b[100];
    CT_CHECK(!slurp(d, "hub_id", b, sizeof b));
    /* the rollback path: the current id is accepted */
    send_msg("{\"v\":1,\"id\":3,\"t\":\"set_hub_id\",\"d\":{\"id\":\"pinned-id\"}}");
    CT_CHECK(has("hub_id_ack"));
}

static void test_no_identity(void) {
    memset(&S, 0, sizeof S);
    memset(&T, 0, sizeof T);
    hub_ctx_init(&H);
    hub_install(&T, &H);
    T.token = "tok";
    T.hub_id = "legacy";
    send_msg("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}");
    CT_CHECK(has("\"hub_id\":\"legacy\"") && !has("rename"));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"set_hub_id\",\"d\":{\"id\":\"x\"}}");
    CT_CHECK(has("\"code\":\"unsupported\""));
}

/* ---- mDNS ---- */
static int fake_detect(uint32_t *ip, uint32_t *mask) {
    (void)ip;
    (void)mask;
    return -1;
}
static uint32_t g_now;
static uint32_t fake_clock(void *c) {
    (void)c;
    return g_now;
}

static void test_mdns(void) {
    hp_mdns_svc_t s;
    CT_EQ(hp_mdns_svc_init(&s, "living-room", 8099, 0x0A000001u, 1), 0);
    strcpy(s.hw, "aabbccddeeff");
    uint8_t buf[512];
    int n = hp_mdns_build_response(&s, HP_MDNS_TXT, 0, 0, buf, sizeof buf);
    CT_CHECK(n > 0);
    CT_CHECK(n > 0 && memmem(buf, (size_t)n, "\x12hub_id=living-room", 19) != NULL);
    CT_CHECK(n > 0 && memmem(buf, (size_t)n, "\x0f" "hw=aabbccddeeff", 16) != NULL);
    s.hw[0] = 0; /* no hardware id: no hw= string at all */
    n = hp_mdns_build_response(&s, HP_MDNS_TXT, 0, 0, buf, sizeof buf);
    CT_CHECK(n > 0 && memmem(buf, (size_t)n, "hw=", 3) == NULL);
    /* rename: new labels, same port and hardware id; an invalid label changes nothing */
    hp_sched_t sc;
    g_now = 0;
    hp_sched_init(&sc, fake_clock, NULL);
    hp_mdns_opts_t o = {0, NULL, "127.0.0.1:9", fake_detect, "aabbccddeeff"};
    hp_mdns_srv_t m;
    CT_EQ(hp_mdns_srv_start(&m, &sc, &o, "old-id", 8099, 1), 0);
    CT_CHECK(!strcmp(m.svc.hw, "aabbccddeeff"));
    CT_EQ(hp_mdns_srv_rename(&m, "new-id"), 0);
    CT_CHECK(!strcmp(m.svc.hub_id, "new-id") && !strcmp(m.svc.inst, "new-id._haharmony._tcp.local.") &&
             !strcmp(m.svc.host, "new-id.local.") && m.svc.port == 8099 && !strcmp(m.svc.hw, "aabbccddeeff"));
    CT_CHECK(hp_mdns_srv_rename(&m, "bad label") < 0);
    CT_CHECK(!strcmp(m.svc.hub_id, "new-id"));
    hp_mdns_srv_stop(&m);
}

/* Upgrade safety: older firmware used "ha-harmony-<hostname>" unvalidated. Wherever that id was usable (the add-on's
 * rule: 1-63 of A-Za-z0-9._-) it must come out byte-identical, and it is saved so it never drifts. */
static void test_old_derivation_is_kept(void) {
    char h63[64], h52[53];
    memset(h63, 'h', 63);
    h63[63] = 0;
    memset(h52, 'h', 52);
    h52[52] = 0; /* "ha-harmony-" + 52 = 63 chars: the longest usable */
    const char *hosts[] = {"hub", "x.lan", "my_host", "Host-9", "ha.rmony.local", h52};
    for (int i = 0; i < 6; i++) {
        char old[120], b[100];
        snprintf(old, sizeof old, "ha-harmony-%s", hosts[i]);
        hp_ident_t t;
        char *d = newdir();
        CT_EQ(hp_ident_init(&t, d, NULL, old, NULL), 0);
        CT_CHECK(!strcmp(t.id, old) && !t.warn[0] && !t.pinned);
        CT_CHECK(slurp(d, "hub_id", b, sizeof b) && !strcmp(b, old)); /* persisted on first boot */
        CT_EQ(hp_ident_init(&t, d, NULL, "ha-harmony-renamed-host", NULL), 0); /* a hostname change cannot move it */
        CT_CHECK(!strcmp(t.id, old));
    }
    /* unusable then: 74 chars, or a space */
    char old[120], b[100];
    snprintf(old, sizeof old, "ha-harmony-%s", h63);
    hp_ident_t t;
    char *d = newdir();
    CT_EQ(hp_ident_init(&t, d, NULL, old, NULL), 0);
    CT_CHECK(strlen(t.id) == 63 && t.warn[0] && slurp(d, "hub_id", b, sizeof b) && !strcmp(b, t.id));
    CT_EQ(hp_ident_init(&t, newdir(), NULL, "ha-harmony-a b", NULL), 0);
    CT_CHECK(!strcmp(t.id, "ha-harmony-a-b"));
    /* an existing dotted id can still change its name */
    CT_EQ(hp_ident_init(&t, newdir(), NULL, "ha-harmony-x.lan", NULL), 0);
    CT_EQ(hp_ident_set(&t, "ha-harmony-x.lan", "Den"), HP_IDENT_OK);
    CT_CHECK(!strcmp(t.id, "ha-harmony-x.lan") && !strcmp(t.name, "Den"));
    CT_EQ(hp_ident_set(&t, "other.dotted", NULL), HP_IDENT_BAD_ID);
}

int main(void) {
    CT_CHECK(mkdtemp(root) != NULL);
    CT_RUN(test_validation);
    CT_RUN(test_precedence_and_persistence);
    CT_RUN(test_old_derivation_is_kept);
    CT_RUN(test_io_failure_changes_nothing);
    CT_RUN(test_hw_read);
    CT_RUN(test_dispatch);
    CT_RUN(test_dispatch_pinned);
    CT_RUN(test_no_identity);
    CT_RUN(test_mdns);
    return CT_DONE();
}
