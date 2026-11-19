/* Bluetooth HID messages (35-38) through the dispatcher: against the fake link (available) and the stub
 * (not available, test_bt_stub). Validation, error mapping, hello_ack.bt, broadcast on change, auth. */
#include <stdio.h>
#include <string.h>

#include "bt_link_fake.h"
#include "ctest_assert.h"
#include "hub/hub_handlers.h"
#include "hub/net/jsonrpc.h"

static hub_ctx_t H;
static hp_handler_tbl_t T;
static hp_session_t S;
static char reply[65536];
static char seen_type[32];
static char seen_json[65536];
static int seen_n;

static void hook(void *ctx, const char *type, const cJSON *d) {
    (void)ctx;
    seen_n++;
    snprintf(seen_type, sizeof seen_type, "%s", type);
    char *p = cJSON_PrintUnformatted(d);
    snprintf(seen_json, sizeof seen_json, "%s", p ? p : "");
    free(p);
}

static void setup(void) {
    memset(&S, 0, sizeof S);
    memset(&T, 0, sizeof T);
    hub_ctx_init(&H);
    bt_fake_reset();
    hub_install(&T, &H);
    T.token = "tok";
    T.hub_id = "h";
    H.bcast_hook = hook;
    seen_n = 0;
}

static void notify(void) {
    bt_fake_notify();
    hub_flush_pending(&H); /* one broadcast per event-loop pass */
}

static void send_msg(const char *json) {
    size_t n = 0;
    reply[0] = 0;
    CT_CHECK(hp_dispatch(json, strlen(json), &T, &S, reply, sizeof reply, &n) >= 0);
    reply[n] = 0;
}
static bool has(const char *needle) { return strstr(reply, needle) != NULL; }
static void hello(void) { send_msg("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}"); }

static void add_host(unsigned slot, const char *addr, const char *name, enum bt_link_transport t, bool conn) {
    struct bt_link_host *h = &g_bt_fake.state.hosts[g_bt_fake.state.n_hosts++];
    h->slot = (uint8_t)slot;
    snprintf(h->addr, sizeof h->addr, "%s", addr);
    snprintf(h->name, sizeof h->name, "%s", name);
    h->transport = t;
    h->connected = conn;
}

static void test_unauthenticated(void) {
    setup();
    send_msg("{\"v\":1,\"id\":1,\"t\":\"bt_pair\",\"d\":{\"on\":true}}");
    CT_CHECK(has("unauthorized"));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_forget\",\"d\":{\"slot\":0}}");
    CT_CHECK(has("unauthorized"));
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(has("unauthorized"));
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_state\",\"d\":{\"state\":\"off\",\"discoverable\":false,\"hosts\":[]}}");
    CT_CHECK(has("unauthorized"));
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_remote_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"down\":true}}");
    CT_CHECK(has("unauthorized"));
    CT_EQ(g_bt_fake.pair_calls + g_bt_fake.forget_calls + g_bt_fake.key_calls, 0);
}

static void test_hello_ack(void) {
    setup();
    hello();
    CT_CHECK(has("\"bt\":{\"hosts_max\":16,\"conn_max\":3,\"remotes_max\":4,\"le_bonds_max\":20,\"pages\":[7,12],\"le\":true}"));
    setup();
    g_bt_fake.limits.hosts_max = 4;
    g_bt_fake.limits.conn_max = 1;
    g_bt_fake.limits.le = false;
    hello();
    CT_CHECK(has("\"hosts_max\":4") && has("\"conn_max\":1") && has("\"le\":false"));
}

static void test_pair(void) {
    setup();
    hello();
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":true}}"); /* defaults: 120 s, any */
    CT_CHECK(has("\"t\":\"bt_state\"") && has("\"id\":2") && has("\"discoverable\":true") && has("\"discoverable_secs\":120") &&
             has("\"state\":\"ready\"") && has("\"hosts\":[]"));
    CT_EQ(g_bt_fake.pair_calls, 1);
    CT_CHECK(g_bt_fake.last_on && g_bt_fake.last_secs == 120 && g_bt_fake.last_transport == BT_TRANSPORT_ANY);
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"secs\":300,\"transport\":\"le\"}}");
    CT_CHECK(has("\"discoverable_secs\":300") && g_bt_fake.last_secs == 300 && g_bt_fake.last_transport == BT_TRANSPORT_LE);
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"secs\":1,\"transport\":\"classic\"}}");
    CT_CHECK(g_bt_fake.last_secs == 1 && g_bt_fake.last_transport == BT_TRANSPORT_CLASSIC);
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"discoverable\":false") && !g_bt_fake.last_on);
    int calls = g_bt_fake.pair_calls;
    send_msg("{\"v\":1,\"id\":6,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"secs\":301}}");
    CT_CHECK(has("bad_secs"));
    send_msg("{\"v\":1,\"id\":7,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"secs\":0}}");
    CT_CHECK(has("bad_secs"));
    send_msg("{\"v\":1,\"id\":8,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"secs\":-5}}");
    CT_CHECK(has("\"t\":\"error\""));
    send_msg("{\"v\":1,\"id\":9,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"transport\":\"zigbee\"}}");
    CT_CHECK(has("bad_transport"));
    send_msg("{\"v\":1,\"id\":10,\"t\":\"bt_pair\",\"d\":{}}"); /* on is required */
    CT_CHECK(has("\"t\":\"error\""));
    send_msg("{\"v\":1,\"id\":11,\"t\":\"bt_pair\",\"d\":{\"on\":\"yes\"}}");
    CT_CHECK(has("\"t\":\"error\""));
    CT_EQ(g_bt_fake.pair_calls, calls);
    g_bt_fake.limits.le = false; /* an adapter without LE refuses an LE-only window */
    send_msg("{\"v\":1,\"id\":12,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"transport\":\"le\"}}");
    CT_CHECK(has("no_le") && g_bt_fake.pair_calls == calls);
    g_bt_fake.next_rc = BT_LINK_E_NOT_READY;
    send_msg("{\"v\":1,\"id\":13,\"t\":\"bt_pair\",\"d\":{\"on\":true}}");
    CT_CHECK(has("bt_not_ready"));
    g_bt_fake.next_rc = BT_LINK_E_BUSY;
    send_msg("{\"v\":1,\"id\":14,\"t\":\"bt_pair\",\"d\":{\"on\":true}}");
    CT_CHECK(has("bt_busy"));
}

static void test_forget_and_slots(void) {
    setup();
    hello();
    add_host(0, "AA:BB:CC:DD:EE:01", "TV", BT_TRANSPORT_CLASSIC, true);
    add_host(15, "AA:BB:CC:DD:EE:02", "", BT_TRANSPORT_LE, false);
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_forget\",\"d\":{\"slot\":15}}"); /* hosts_max 16: slot 15 is valid */
    CT_CHECK(has("\"t\":\"bt_state\"") && g_bt_fake.forget_calls == 1 && g_bt_fake.last_slot == 15);
    CT_CHECK(has("\"slot\":0") && has("\"name\":\"TV\"") && has("\"transport\":\"classic\"") && has("\"connected\":true") &&
             !has("EE:02"));
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_forget\",\"d\":{\"slot\":16}}"); /* >= hosts_max: refused before the link */
    CT_CHECK(has("no_slot") && g_bt_fake.forget_calls == 1);
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_forget\",\"d\":{\"slot\":7}}"); /* in range, nothing paired there */
    CT_CHECK(has("no_slot") && g_bt_fake.forget_calls == 2);
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_forget\",\"d\":{\"slot\":256}}"); /* does not fit u8 */
    CT_CHECK(has("\"t\":\"error\"") && g_bt_fake.forget_calls == 2);
    send_msg("{\"v\":1,\"id\":6,\"t\":\"bt_forget\",\"d\":{\"slot\":-1}}");
    CT_CHECK(has("\"t\":\"error\"") && g_bt_fake.forget_calls == 2);
    send_msg("{\"v\":1,\"id\":7,\"t\":\"bt_forget\",\"d\":{}}");
    CT_CHECK(has("\"t\":\"error\""));
    setup();
    hello();
    g_bt_fake.limits.hosts_max = 4; /* the limit comes from the link, not a constant */
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_forget\",\"d\":{\"slot\":4}}");
    CT_CHECK(has("no_slot") && g_bt_fake.forget_calls == 0);
}

static void test_key(void) {
    setup();
    hello();
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_key\",\"d\":{\"slot\":3,\"page\":12,\"usage\":233,\"action\":\"press\"}}");
    CT_CHECK(has("\"t\":\"command_result\"") && has("\"ok\":true"));
    CT_CHECK(g_bt_fake.key_calls == 1 && g_bt_fake.last_slot == 3 && g_bt_fake.last_page == 12 &&
             g_bt_fake.last_usage == 233 && g_bt_fake.last_action == BT_KEY_PRESS);
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":65535,\"action\":\"down\"}}");
    CT_CHECK(g_bt_fake.last_usage == 65535 && g_bt_fake.last_action == BT_KEY_DOWN && g_bt_fake.last_page == 7);
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"up\"}}");
    CT_CHECK(g_bt_fake.last_action == BT_KEY_UP);
    int calls = g_bt_fake.key_calls;
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":9,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(has("bad_page"));
    send_msg("{\"v\":1,\"id\":6,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"tap\"}}");
    CT_CHECK(has("bad_action"));
    send_msg("{\"v\":1,\"id\":7,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":65536,\"action\":\"press\"}}");
    CT_CHECK(has("\"t\":\"error\""));
    send_msg("{\"v\":1,\"id\":8,\"t\":\"bt_key\",\"d\":{\"slot\":16,\"page\":7,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(has("no_slot"));
    send_msg("{\"v\":1,\"id\":9,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4}}");
    CT_CHECK(has("\"t\":\"error\""));
    CT_EQ(g_bt_fake.key_calls, calls);
    g_bt_fake.next_rc = BT_LINK_E_NOT_CONNECTED;
    send_msg("{\"v\":1,\"id\":10,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(has("not_connected"));
}

static void test_state_json_and_broadcast(void) {
    setup();
    hello();
    g_bt_fake.state.status = BT_READY;
    g_bt_fake.state.pairing = BT_PAIR_OK;
    snprintf(g_bt_fake.state.pairing_addr, BT_LINK_ADDR_MAX, "AA:BB:CC:DD:EE:03");
    snprintf(g_bt_fake.state.pairing_name, BT_LINK_NAME_MAX, "Phone");
    g_bt_fake.state.pairing_transport = BT_TRANSPORT_LE;
    add_host(2, "AA:BB:CC:DD:EE:03", "Phone", BT_TRANSPORT_LE, true);
    notify(); /* a change on the link reaches the broadcast path */
    CT_EQ(seen_n, 1);
    CT_CHECK(!strcmp(seen_type, "bt_state"));
    CT_CHECK(strstr(seen_json, "\"pairing\":{\"result\":\"ok\",\"addr\":\"AA:BB:CC:DD:EE:03\",\"name\":\"Phone\",\"kind\":\"host\",\"transport\":\"le\"}"));
    CT_CHECK(strstr(seen_json, "\"hosts\":[{\"slot\":2,\"addr\":\"AA:BB:CC:DD:EE:03\",\"name\":\"Phone\",\"transport\":\"le\",\"connected\":true}]"));
    g_bt_fake.state.pairing = BT_PAIR_REFUSED;
    g_bt_fake.state.pairing_name[0] = 0;
    g_bt_fake.state.pairing_transport = BT_TRANSPORT_ANY;
    g_bt_fake.state.status = BT_NO_ADAPTER;
    notify();
    CT_CHECK(strstr(seen_json, "\"state\":\"no_adapter\"") && strstr(seen_json, "\"result\":\"refused\"") &&
             !strstr(seen_json, "\"name\":\"\"") && !strstr(seen_json, "\"transport\":\"le\"}"));
    static const char *const names[] = {"off", "starting", "ready", "no_adapter", "disabled"};
    for (int i = 0; i < 5; i++) {
        g_bt_fake.state.status = (enum bt_link_status)i;
        notify();
        char want[40];
        snprintf(want, sizeof want, "\"state\":\"%s\"", names[i]);
        CT_CHECK(strstr(seen_json, want));
    }
    /* a full house still fits one frame */
    g_bt_fake.state.n_hosts = 0;
    for (unsigned i = 0; i < 16; i++) add_host(i, "AA:BB:CC:DD:EE:FF", "A host with a longish name 0123456789", i & 1 ? BT_TRANSPORT_LE : BT_TRANSPORT_CLASSIC, true);
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"t\":\"bt_state\"") && has("\"slot\":15"));
}

static void test_remotes(void) {
    setup();
    hello();
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"remote\",\"secs\":60}}");
    CT_CHECK(has("\"t\":\"bt_state\"") && g_bt_fake.last_mode == BT_MODE_REMOTE && g_bt_fake.last_secs == 60 &&
             g_bt_fake.last_transport == BT_TRANSPORT_ANY && has("\"remotes\":[]"));
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_pair\",\"d\":{\"on\":true}}"); /* default mode: host */
    CT_CHECK(g_bt_fake.last_mode == BT_MODE_HOST);
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"remote\",\"transport\":\"le\"}}");
    CT_CHECK(g_bt_fake.last_mode == BT_MODE_REMOTE && g_bt_fake.last_transport == BT_TRANSPORT_LE);
    int calls = g_bt_fake.pair_calls;
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"remote\",\"transport\":\"classic\"}}");
    CT_CHECK(has("bad_transport"));
    send_msg("{\"v\":1,\"id\":6,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"tv\"}}");
    CT_CHECK(has("bad_mode"));
    g_bt_fake.limits.remotes_max = 0;
    send_msg("{\"v\":1,\"id\":7,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"remote\"}}");
    CT_CHECK(has("unsupported"));
    g_bt_fake.limits.remotes_max = 4;
    g_bt_fake.limits.le = false;
    send_msg("{\"v\":1,\"id\":8,\"t\":\"bt_pair\",\"d\":{\"on\":true,\"mode\":\"remote\"}}");
    CT_CHECK(has("no_le"));
    CT_EQ(g_bt_fake.pair_calls, calls);
    g_bt_fake.limits.le = true;

    /* state carries remotes and the pairing kind */
    struct bt_link_remote *r = &g_bt_fake.state.remotes[g_bt_fake.state.n_remotes++];
    r->slot = 1;
    snprintf(r->addr, sizeof r->addr, "11:22:33:44:55:66");
    snprintf(r->name, sizeof r->name, "Clicker");
    r->connected = true;
    r->battery = 80;
    r = &g_bt_fake.state.remotes[g_bt_fake.state.n_remotes++];
    r->slot = 3;
    snprintf(r->addr, sizeof r->addr, "11:22:33:44:55:77");
    r->battery = -1;
    g_bt_fake.state.pairing = BT_PAIR_OK;
    g_bt_fake.state.pairing_kind = BT_KIND_REMOTE;
    snprintf(g_bt_fake.state.pairing_addr, BT_LINK_ADDR_MAX, "11:22:33:44:55:66");
    notify();
    CT_CHECK(strstr(seen_json, "\"kind\":\"remote\""));
    CT_CHECK(strstr(seen_json, "\"remotes\":[{\"slot\":1,\"addr\":\"11:22:33:44:55:66\",\"name\":\"Clicker\",\"connected\":true,\"battery\":80},"
                               "{\"slot\":3,\"addr\":\"11:22:33:44:55:77\",\"connected\":false}]"));

    /* forget kind: remote slots are bounded by remotes_max, not hosts_max */
    send_msg("{\"v\":1,\"id\":9,\"t\":\"bt_forget\",\"d\":{\"slot\":3,\"kind\":\"remote\"}}");
    CT_CHECK(has("\"t\":\"bt_state\"") && g_bt_fake.last_kind == BT_KIND_REMOTE && !has("55:77"));
    send_msg("{\"v\":1,\"id\":10,\"t\":\"bt_forget\",\"d\":{\"slot\":4,\"kind\":\"remote\"}}");
    CT_CHECK(has("no_slot"));
    send_msg("{\"v\":1,\"id\":11,\"t\":\"bt_forget\",\"d\":{\"slot\":0,\"kind\":\"watch\"}}");
    CT_CHECK(has("bad_kind"));
    send_msg("{\"v\":1,\"id\":12,\"t\":\"bt_forget\",\"d\":{\"slot\":9}}"); /* default kind host: 9 < 16 */
    CT_CHECK(g_bt_fake.last_kind == BT_KIND_HOST && has("no_slot") && g_bt_fake.forget_calls == 2);
}

static void test_remote_key_push(void) {
    setup();
    hello();
    bt_fake_remote_key(1, 12, 233, true); /* hub-side: always offered to the broadcast path (filtered per socket) */
    CT_CHECK(seen_n == 1 && !strcmp(seen_type, "bt_remote_key") &&
             !strcmp(seen_json, "{\"slot\":1,\"page\":12,\"usage\":233,\"down\":true}"));
    CT_CHECK(!S.bt_listen);
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_listen\",\"d\":{\"on\":true}}");
    CT_CHECK(has("\"t\":\"command_result\"") && has("\"ok\":true") && S.bt_listen);
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_listen\",\"d\":{\"on\":false}}");
    CT_CHECK(!S.bt_listen);
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_listen\",\"d\":{\"on\":true}}");
    hello(); /* a new hello resets the opt-in */
    CT_CHECK(!S.bt_listen);
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_listen\",\"d\":{}}");
    CT_CHECK(has("\"t\":\"error\""));
    setup(); /* unauthenticated */
    send_msg("{\"v\":1,\"id\":1,\"t\":\"bt_listen\",\"d\":{\"on\":true}}");
    CT_CHECK(has("unauthorized") && !S.bt_listen);
    g_bt_fake.available = false;
    hello();
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_listen\",\"d\":{\"on\":true}}");
    CT_CHECK(has("unsupported") && !S.bt_listen);
}

static void test_max_hosts_fit_one_frame(void) {
    setup();
    hello();
    g_bt_fake.limits.hosts_max = 255;
    g_bt_fake.limits.remotes_max = 32;
    char worst[BT_LINK_NAME_MAX]; /* 48 bytes that each escape to 6 */
    memset(worst, 0x01, 48);
    worst[48] = 0;
    for (unsigned i = 0; i < 255; i++) add_host(i, "AA:BB:CC:DD:EE:FF", worst, BT_TRANSPORT_LE, true);
    for (unsigned i = 0; i < 32; i++) {
        struct bt_link_remote *r = &g_bt_fake.state.remotes[g_bt_fake.state.n_remotes++];
        r->slot = (uint8_t)i;
        snprintf(r->addr, sizeof r->addr, "11:22:33:44:55:66");
        memcpy(r->name, worst, sizeof worst);
        r->battery = 100;
    }
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"t\":\"bt_state\"") && has("\"slot\":254"));
    CT_CHECK(strlen(reply) < 65536);
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_forget\",\"d\":{\"slot\":254}}"); /* valid: hosts_max is 255 */
    CT_CHECK(has("\"t\":\"bt_state\""));
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_forget\",\"d\":{\"slot\":255}}");
    CT_CHECK(has("no_slot"));
    send_msg("{\"v\":1,\"id\":5,\"t\":\"bt_forget\",\"d\":{\"slot\":31,\"kind\":\"remote\"}}");
    CT_CHECK(has("\"t\":\"bt_state\""));
    notify(); /* the broadcast path takes the same size */
    CT_CHECK(seen_n == 1 && strlen(seen_json) > 0);
}

static void test_untrusted_link_data(void) {
    setup();
    hello();
    struct bt_link_state *st = &g_bt_fake.state;
    memset(st->hosts[0].name, 'x', BT_LINK_NAME_MAX); /* not NUL terminated */
    snprintf(st->hosts[0].addr, sizeof st->hosts[0].addr, "AA:BB:CC:DD:EE:01");
    st->hosts[1].slot = 1;
    snprintf(st->hosts[1].addr, sizeof st->hosts[1].addr, "AA:BB:CC:DD:EE:02");
    snprintf(st->hosts[1].name, sizeof st->hosts[1].name, "bad\xff\xfe");
    st->hosts[2].slot = 2;
    snprintf(st->hosts[2].addr, sizeof st->hosts[2].addr, "not an address!!!");
    st->hosts[3].slot = 3;
    snprintf(st->hosts[3].addr, sizeof st->hosts[3].addr, "AA:BB:CC:DD:EE:0\"");
    st->hosts[4].slot = 4;
    snprintf(st->hosts[4].addr, sizeof st->hosts[4].addr, "aa:bb:cc:dd:ee:05");
    st->n_hosts = 5;
    st->remotes[0].slot = 0;
    snprintf(st->remotes[0].addr, sizeof st->remotes[0].addr, "AABBCCDDEE0000000");
    st->n_remotes = 1;
    st->status = (enum bt_link_status)200;
    st->pairing = (enum bt_link_pair_result)77;
    snprintf(st->pairing_addr, sizeof st->pairing_addr, "AA:BB:CC:DD:EE:09");
    st->hosts[0].transport = (enum bt_link_transport)9;
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"state\":\"off\"") && !has("\"pairing\""));
    CT_CHECK(has("\"slot\":0") && has("\"slot\":1") && has("\"slot\":4") && !has("\"slot\":2") && !has("\"slot\":3"));
    CT_CHECK(!has("\\u00") && !has("\xff") && !has("bad")); /* bad UTF-8 name omitted */
    CT_CHECK(has("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"") && !has("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    CT_CHECK(has("\"remotes\":[]"));
    st->status = BT_READY;
    st->pairing = BT_PAIR_OK;
    st->pairing_transport = (enum bt_link_transport)9;
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"result\":\"ok\"") && !has("\"transport\":\"le\"}"));
}

static void test_gen_and_coalescing_and_hello_state(void) {
    setup();
    hello();
    g_bt_fake.state.gen = 41;
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":false}}");
    CT_CHECK(has("\"gen\":41"));
    /* a burst of changes: one broadcast per pass; no change: none */
    g_bt_fake.state.gen = 44;
    bt_fake_notify();
    bt_fake_notify();
    bt_fake_notify();
    CT_EQ(seen_n, 0);
    hub_flush_pending(&H);
    CT_EQ(seen_n, 1);
    CT_CHECK(strstr(seen_json, "\"gen\":44"));
    hub_flush_pending(&H);
    CT_EQ(seen_n, 1);
    /* the state frame sent right after hello */
    char frame[65536];
    size_t n = 0;
    CT_EQ(hub_bt_state_frame(frame, sizeof frame, &n), 0);
    frame[n] = 0;
    CT_CHECK(strstr(frame, "\"t\":\"bt_state\"") && strstr(frame, "\"id\":0") && strstr(frame, "\"gen\":44"));
    g_bt_fake.available = false;
    CT_CHECK(hub_bt_state_frame(frame, sizeof frame, &n) != 0);
}

static void test_unavailable(void) {
    setup();
    g_bt_fake.available = false;
    hello();
    CT_CHECK(has("\"t\":\"hello_ack\"") && !has("\"bt\""));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":true}}");
    CT_CHECK(has("unsupported") && g_bt_fake.pair_calls == 0);
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_forget\",\"d\":{\"slot\":0}}");
    CT_CHECK(has("unsupported"));
    send_msg("{\"v\":1,\"id\":4,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(has("unsupported") && g_bt_fake.key_calls == 0);
}

int main(void) {
    CT_RUN(test_unauthenticated);
    CT_RUN(test_hello_ack);
    CT_RUN(test_pair);
    CT_RUN(test_forget_and_slots);
    CT_RUN(test_key);
    CT_RUN(test_state_json_and_broadcast);
    CT_RUN(test_remotes);
    CT_RUN(test_remote_key_push);
    CT_RUN(test_max_hosts_fit_one_frame);
    CT_RUN(test_untrusted_link_data);
    CT_RUN(test_gen_and_coalescing_and_hello_state);
    CT_RUN(test_unavailable);
    return CT_DONE();
}
