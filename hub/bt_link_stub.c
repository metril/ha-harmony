/* bt_link for builds without Bluetooth: never available, state "off". */
#include <string.h>

#include "hub/bt_link.h"

bool bt_link_available(void) { return false; }
int bt_link_pair(bool on, unsigned secs, enum bt_link_transport transport, enum bt_link_pair_mode mode) {
    (void)on;
    (void)secs;
    (void)transport;
    (void)mode;
    return BT_LINK_E_NOT_READY;
}
int bt_link_pair_remote(const char *addr) {
    (void)addr;
    return BT_LINK_E_NOT_READY;
}
int bt_link_forget(enum bt_link_kind kind, unsigned slot) {
    (void)kind;
    (void)slot;
    return BT_LINK_E_NOT_READY;
}
int bt_link_key(unsigned slot, unsigned page, unsigned usage, enum bt_link_action action) {
    (void)slot;
    (void)page;
    (void)usage;
    (void)action;
    return BT_LINK_E_NOT_READY;
}
void bt_link_limits(struct bt_link_limits *out) { memset(out, 0, sizeof *out); }
void bt_link_state(struct bt_link_state *out) {
    memset(out, 0, sizeof *out);
    out->status = BT_OFF;
}
void bt_link_set_listener(void (*cb)(void *ctx), void *ctx) {
    (void)cb;
    (void)ctx;
}
void bt_link_set_remote_key_cb(void (*cb)(void *ctx, unsigned slot, unsigned page, unsigned usage, bool down), void *ctx) {
    (void)cb;
    (void)ctx;
}
