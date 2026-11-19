#include "bt_link_fake.h"

#include <string.h>

struct bt_fake g_bt_fake;

void bt_fake_reset(void) {
    void (*l)(void *) = g_bt_fake.listener;
    void *lc = g_bt_fake.listener_ctx;
    void (*rk)(void *, unsigned, unsigned, unsigned, bool) = g_bt_fake.rkey;
    void *rkc = g_bt_fake.rkey_ctx;
    memset(&g_bt_fake, 0, sizeof g_bt_fake);
    g_bt_fake.listener = l;
    g_bt_fake.listener_ctx = lc;
    g_bt_fake.rkey = rk;
    g_bt_fake.rkey_ctx = rkc;
    g_bt_fake.available = true;
    g_bt_fake.limits.hosts_max = 16;
    g_bt_fake.limits.conn_max = 3;
    g_bt_fake.limits.remotes_max = 4;
    g_bt_fake.limits.le = true;
    g_bt_fake.state.status = BT_READY;
    g_bt_fake.state.le = true;
}

void bt_fake_notify(void) {
    if (g_bt_fake.listener) g_bt_fake.listener(g_bt_fake.listener_ctx);
}

bool bt_link_available(void) { return g_bt_fake.available; }

int bt_link_pair(bool on, unsigned secs, enum bt_link_transport transport, enum bt_link_pair_mode mode) {
    g_bt_fake.last_mode = mode;
    g_bt_fake.pair_calls++;
    g_bt_fake.last_on = on;
    g_bt_fake.last_secs = secs;
    g_bt_fake.last_transport = transport;
    if (g_bt_fake.next_rc) return g_bt_fake.next_rc;
    g_bt_fake.state.discoverable = on;
    g_bt_fake.state.discoverable_secs = on ? (uint16_t)secs : 0;
    return BT_LINK_OK;
}

int bt_link_forget(enum bt_link_kind kind, unsigned slot) {
    g_bt_fake.forget_calls++;
    g_bt_fake.last_kind = kind;
    g_bt_fake.last_slot = slot;
    if (g_bt_fake.next_rc) return g_bt_fake.next_rc;
    struct bt_link_state *s = &g_bt_fake.state;
    if (kind == BT_KIND_REMOTE) {
        for (unsigned i = 0; i < s->n_remotes; i++) {
            if (s->remotes[i].slot != slot) continue;
            memmove(&s->remotes[i], &s->remotes[i + 1], (s->n_remotes - i - 1) * sizeof s->remotes[0]);
            s->n_remotes--;
            return BT_LINK_OK;
        }
        return BT_LINK_E_NO_SLOT;
    }
    for (unsigned i = 0; i < s->n_hosts; i++) {
        if (s->hosts[i].slot != slot) continue;
        memmove(&s->hosts[i], &s->hosts[i + 1], (s->n_hosts - i - 1) * sizeof s->hosts[0]);
        s->n_hosts--;
        return BT_LINK_OK;
    }
    return BT_LINK_E_NO_SLOT;
}

int bt_link_key(unsigned slot, unsigned page, unsigned usage, enum bt_link_action action) {
    g_bt_fake.key_calls++;
    g_bt_fake.last_slot = slot;
    g_bt_fake.last_page = page;
    g_bt_fake.last_usage = usage;
    g_bt_fake.last_action = action;
    return g_bt_fake.next_rc;
}

void bt_link_limits(struct bt_link_limits *out) { *out = g_bt_fake.limits; }
void bt_link_state(struct bt_link_state *out) { *out = g_bt_fake.state; }

void bt_link_set_listener(void (*cb)(void *ctx), void *ctx) {
    g_bt_fake.listener = cb;
    g_bt_fake.listener_ctx = ctx;
}

void bt_link_set_remote_key_cb(void (*cb)(void *ctx, unsigned slot, unsigned page, unsigned usage, bool down), void *ctx) {
    g_bt_fake.rkey = cb;
    g_bt_fake.rkey_ctx = ctx;
}

void bt_fake_remote_key(unsigned slot, unsigned page, unsigned usage, bool down) {
    if (g_bt_fake.rkey) g_bt_fake.rkey(g_bt_fake.rkey_ctx, slot, page, usage, down);
}
