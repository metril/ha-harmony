#ifndef BT_LINK_FAKE_H
#define BT_LINK_FAKE_H
/* Test double for hub/bt_link.h (replaces hub/bt_link_stub.c in the test executable): an "available" link whose
 * state the test sets directly, that records every request, and fires the listener on demand. */
#include "hub/bt_link.h"

struct bt_fake {
    bool available;
    struct bt_link_limits limits;
    struct bt_link_state state;
    int next_rc;  /* returned by pair/forget/key while non-zero */
    int pair_remote_calls;
    char last_pick[BT_LINK_ADDR_MAX];
    int pair_calls,forget_calls, key_calls;
    bool last_on;
    enum bt_link_pair_mode last_mode;
    enum bt_link_kind last_kind;
    unsigned last_secs, last_slot, last_page, last_usage;
    enum bt_link_transport last_transport;
    enum bt_link_action last_action;
    void (*listener)(void *);
    void *listener_ctx;
    void (*rkey)(void *, unsigned, unsigned, unsigned, bool);
    void *rkey_ctx;
};

extern struct bt_fake g_bt_fake;
void bt_fake_reset(void);       /* available, hosts_max 16, conn_max 3, remotes_max 4, le, state ready, no hosts */
void bt_fake_remote_key(unsigned slot, unsigned page, unsigned usage, bool down); /* a bonded remote sent a key */
void bt_fake_notify(void);      /* the link reports a state change */

#endif
