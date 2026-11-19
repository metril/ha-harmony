/* A build without Bluetooth (hub/bt_link_stub.c): hello_ack has no `bt`, bt_* requests are refused, state is off. */
#include <string.h>

#include "ctest_assert.h"
#include "hub/bt_link.h"
#include "hub/hub_handlers.h"
#include "hub/net/jsonrpc.h"

static hub_ctx_t H;
static hp_handler_tbl_t T;
static hp_session_t S;
static char reply[4096];

static void send_msg(const char *json) {
    size_t n = 0;
    reply[0] = 0;
    CT_CHECK(hp_dispatch(json, strlen(json), &T, &S, reply, sizeof reply, &n) >= 0);
    reply[n] = 0;
}

int main(void) {
    hub_ctx_init(&H);
    hub_install(&T, &H);
    T.token = "tok";
    T.hub_id = "h";
    CT_CHECK(!bt_link_available());
    struct bt_link_state st;
    memset(&st, 0xff, sizeof st);
    bt_link_state(&st);
    CT_CHECK(st.status == BT_OFF && st.n_hosts == 0 && !st.discoverable && st.pairing == BT_PAIR_NONE);
    send_msg("{\"v\":1,\"id\":1,\"t\":\"hello\",\"d\":{\"token\":\"tok\"}}");
    CT_CHECK(strstr(reply, "\"t\":\"hello_ack\"") && !strstr(reply, "\"bt\""));
    send_msg("{\"v\":1,\"id\":2,\"t\":\"bt_pair\",\"d\":{\"on\":true}}");
    CT_CHECK(strstr(reply, "unsupported"));
    send_msg("{\"v\":1,\"id\":3,\"t\":\"bt_key\",\"d\":{\"slot\":0,\"page\":7,\"usage\":4,\"action\":\"press\"}}");
    CT_CHECK(strstr(reply, "unsupported"));
    return CT_DONE();
}
