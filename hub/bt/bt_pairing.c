/* Hosts: pairing window, connect-on-demand (Classic page loop, LE advertising), the connection
 * table (least recently used idle connection is dropped when it is full), forget. */
#include "bt_pairing.h"

#include "bt_hid.h"
#include "bt_hid_queue.h"
#include "bt_reconnect_queue.h"
#include "bt_remote.h"

bool g_classic_paging; /* a page attempt runs: LE scanning pauses */

static btstack_timer_source_t s_win_timer, s_loop_timer;
static bool s_new_key_valid;
static bd_addr_t s_new_key;
static int s_loop_slot = -1, s_loop_attempts;
static bool s_connecting;
static uint32_t s_conn_since;

static uint32_t now_ms(void) { return btstack_run_loop_get_time_ms(); }

/* ---- pairing window ---- */

bool bt_pair_host_open(void) { return g_window == 1; }
bool bt_pair_remote_open(void) { return g_window == 2; }

static void window_closed(void) {
    btstack_run_loop_remove_timer(&s_win_timer);
    gap_discoverable_control(0);
    s_new_key_valid = false;
    g_window = 0;
    bt_adv_update();
    bt_remote_scan_update();
    bt_push_state();
}

/* the open window ends with this result (success, failure or timeout) */
void bt_pair_finish(uint8_t kind, uint8_t result, int slot) {
    if (g_window) window_closed();
    bt_push_pairres(kind, result, slot);
}

static void win_timeout(btstack_timer_source_t *ts) {
    (void)ts;
    uint8_t kind = g_window == 2 ? BT_KIND_REMOTE : BT_KIND_HOST;
    log_msg("pairing window timed out");
    bt_pair_finish(kind, BT_PR_TIMEOUT, BT_SLOT_NONE);
}

void bt_pair_window(bool on, uint16_t secs, uint8_t mode) {
    if (!on) {
        if (g_window) bt_pair_finish(g_window == 2 ? BT_KIND_REMOTE : BT_KIND_HOST, BT_PR_CLOSED, BT_SLOT_NONE);
        return;
    }
    if (g_bt_state != BT_ST_READY) return;
    if (g_window) window_closed();
    if (mode == BT_KIND_REMOTE && !g_bt_le) {
        bt_push_pairres(BT_KIND_REMOTE, BT_PR_FAILED, BT_SLOT_NONE); /* no LE: nothing to scan with */
        return;
    }
    if (secs == 0) secs = BT_PAIR_DEFAULT_S;
    if (secs > BT_PAIR_MAX_S) secs = BT_PAIR_MAX_S;
    g_window = mode == BT_KIND_REMOTE ? 2 : 1;
    s_new_key_valid = false;
    if (g_window == 1) gap_discoverable_control(1);
    btstack_run_loop_set_timer_handler(&s_win_timer, win_timeout);
    btstack_run_loop_set_timer(&s_win_timer, (uint32_t)secs * 1000u);
    btstack_run_loop_add_timer(&s_win_timer);
    bt_logf("pairing window open (%s, %u s)", g_window == 1 ? "host" : "remote", secs);
    bt_adv_update();
    bt_remote_scan_update();
    bt_push_state();
}

/* ---- connection table ---- */

static void drop_conn(int s) {
    bt_host_t *h = &g_hosts[s];
    if (!h->connected) return;
    bt_hid_disconnected(s);
    if (h->transport == BT_TR_CLASSIC && h->cid) hid_device_disconnect(h->cid);
    if (h->handle != HCI_CON_HANDLE_INVALID) gap_disconnect(h->handle);
    h->connected = false;
    h->cid = 0;
    h->handle = HCI_CON_HANDLE_INVALID;
}

/* Room for one more connection: the least recently used idle one makes way */
static void make_room(void) {
    if (bt_hosts_connected() < HH_BT_MAX_CONN) return;
    int victim = -1;
    for (int i = 0; i < g_hosts_n; i++) {
        if (!g_hosts[i].used || !g_hosts[i].connected || !bt_hid_slot_idle(i)) continue;
        if (victim < 0 || (int32_t)(g_hosts[i].last_used_ms - g_hosts[victim].last_used_ms) < 0) victim = i;
    }
    if (victim < 0) {
        log_msg("connection table full and nothing idle");
        return;
    }
    bt_logf("connection table full: dropping idle slot %d", victim);
    drop_conn(victim);
    bt_push_hosts();
}

/* ---- Classic: page loop ---- */

static bool rq_skip(const uint8_t a[6]) {
    int s = bt_host_find_addr(a, BT_TR_CLASSIC);
    return s < 0 || g_hosts[s].connected;
}

static void loop_start(int slot);

static void loop_next(void) {
    btstack_run_loop_remove_timer(&s_loop_timer);
    s_loop_slot = -1;
    s_connecting = false;
    g_classic_paging = false;
    bt_remote_scan_update();
    uint8_t a[6];
    while (rq_pop(a, rq_skip)) {
        int s = bt_host_find_addr(a, BT_TR_CLASSIC);
        if (s >= 0) {
            loop_start(s);
            return;
        }
    }
}

static void classic_try(int slot) {
    bt_host_t *h = &g_hosts[slot];
    if (h->connected) return;
    if (s_connecting && now_ms() - s_conn_since < 5000) return;
    uint16_t cid = 0;
    uint8_t st = hid_device_connect(h->addr, &cid);
    if (st != ERROR_CODE_SUCCESS || cid == 0) {
        bt_logf("slot %d: connect request failed (0x%02x)", slot, st);
        return;
    }
    s_connecting = true;
    s_conn_since = now_ms();
    g_classic_paging = true;
    bt_remote_scan_update();
}

static void loop_timer(btstack_timer_source_t *ts) {
    (void)ts;
    if (s_loop_slot < 0) return;
    bt_host_t *h = &g_hosts[s_loop_slot];
    if (!h->used || h->connected) {
        loop_next();
        return;
    }
    if (++s_loop_attempts > BT_CLASSIC_ATTEMPTS) {
        bt_logf("slot %d: giving up after %d attempts", s_loop_slot, BT_CLASSIC_ATTEMPTS);
        bt_hid_drop_deferred(s_loop_slot);
        loop_next();
        return;
    }
    classic_try(s_loop_slot);
    btstack_run_loop_set_timer(&s_loop_timer, s_loop_attempts <= 5 ? 1000 : 2000);
    btstack_run_loop_add_timer(&s_loop_timer);
}

static void loop_start(int slot) {
    bt_host_t *h = &g_hosts[slot];
    if (hci_get_state() != HCI_STATE_WORKING) {
        rq_push(h->addr);
        return;
    }
    if (s_loop_slot == slot) return;
    /* one loop at a time: the host asked for last goes first, the replaced one waits its turn */
    if (s_loop_slot >= 0) rq_push(g_hosts[s_loop_slot].addr);
    rq_remove(h->addr);
    s_loop_slot = slot;
    s_loop_attempts = 0;
    s_connecting = false;
    bt_logf("slot %d: connecting", slot);
    classic_try(slot);
    btstack_run_loop_remove_timer(&s_loop_timer);
    btstack_run_loop_set_timer_handler(&s_loop_timer, loop_timer);
    btstack_run_loop_set_timer(&s_loop_timer, 1000);
    btstack_run_loop_add_timer(&s_loop_timer);
}

void bt_classic_link_key(const bd_addr_t addr) {
    if (g_window == 1) {
        memcpy(s_new_key, addr, 6);
        s_new_key_valid = true;
    }
}

void bt_classic_name(const bd_addr_t addr, const char *name) {
    int s = bt_host_find_addr(addr, BT_TR_CLASSIC);
    if (s < 0 || !name[0]) return;
    snprintf(g_hosts[s].name, sizeof g_hosts[s].name, "%s", name);
    bt_host_save(s);
    bt_push_hosts();
}

void bt_classic_hid_opened(uint8_t status, uint16_t cid, const bd_addr_t addr, hci_con_handle_t handle) {
    s_connecting = false;
    g_classic_paging = false;
    bt_remote_scan_update();
    int s = bt_host_find_addr(addr, BT_TR_CLASSIC);
    if (status != ERROR_CODE_SUCCESS) {
        bt_logf("Classic HID open to %s failed (0x%02x)", bd_addr_to_str(addr), status);
        if (s >= 0) g_hosts[s].cid = 0; /* the loop tries again */
        if (g_window == 1 && s < 0) return;
        return;
    }
    if (cid == 0) { /* a link the stack cannot address: drop it, the next attempt opens a clean one */
        if (handle != HCI_CON_HANDLE_INVALID) gap_disconnect(handle);
        return;
    }
    bool is_new = false;
    if (s < 0) {
        if (g_window == 1 && s_new_key_valid && memcmp(s_new_key, addr, 6) == 0) {
            s = bt_host_alloc(BT_TR_CLASSIC, 0, addr, "");
            if (s < 0) {
                log_msg("pairing: host table full");
                gap_drop_link_key_for_bd_addr((uint8_t *)addr);
                hid_device_disconnect(cid);
                bt_pair_finish(BT_KIND_HOST, BT_PR_FULL, BT_SLOT_NONE);
                return;
            }
            is_new = true;
        } else {
            bt_logf("Classic HID from unknown host %s refused", bd_addr_to_str(addr));
            hid_device_disconnect(cid);
            return;
        }
    }
    make_room();
    bt_host_t *h = &g_hosts[s];
    h->connected = true;
    h->cid = cid;
    h->handle = handle;
    h->last_used_ms = now_ms();
    bt_logf("slot %d: Classic host %s connected", s, bd_addr_to_str(addr));
    if (is_new) {
        bt_host_save(s);
        gap_remote_name_request((uint8_t *)addr, 0, 0);
    } else if (!h->name[0]) {
        gap_remote_name_request((uint8_t *)addr, 0, 0);
    }
    rq_remove(addr);
    bt_hid_connected(s, false);
    if (s_loop_slot == s) loop_next();
    bt_push_hosts();
    if (is_new) bt_pair_finish(BT_KIND_HOST, BT_PR_OK, s);
}

static void classic_gone(int s) {
    if (s < 0) return;
    bt_hid_disconnected(s);
    g_hosts[s].connected = false;
    g_hosts[s].cid = 0;
    g_hosts[s].handle = HCI_CON_HANDLE_INVALID;
    bt_logf("slot %d: disconnected", s);
    bt_push_hosts();
}

void bt_classic_hid_closed(uint16_t cid) { classic_gone(bt_host_find_cid(cid)); }
void bt_classic_acl_gone(hci_con_handle_t handle) {
    int s = bt_host_find_handle(handle);
    if (s >= 0 && g_hosts[s].transport == BT_TR_CLASSIC) classic_gone(s);
}

/* ---- LE peripheral (HID over GATT) ---- */

#ifdef HH_BT_LE
static btstack_timer_source_t s_adv_timer;
static bool s_adv_on;
static uint32_t s_le_reconn_until;
static hci_con_handle_t s_pending[HH_BT_MAX_CONN + 2];
static uint8_t s_adv_data[] = {2, 0x01, 0x06, 3, 0x03, 0x12, 0x18, 3, 0x19, 0xC1, 0x03,
                               12, 0x09, 'H', 'a', 'r', 'm', 'o', 'n', 'y', ' ', 'H', 'u', 'b'};

static void adv_timer(btstack_timer_source_t *ts) {
    (void)ts;
    bt_adv_update();
}

void bt_adv_update(void) {
    if (!g_bt_le) return;
    bool reconn = s_le_reconn_until && (int32_t)(s_le_reconn_until - now_ms()) > 0;
    if (!reconn) s_le_reconn_until = 0;
    bool want = g_bt_state == BT_ST_READY && (g_window == 1 || reconn) && bt_hosts_connected() < HH_BT_MAX_CONN;
    btstack_run_loop_remove_timer(&s_adv_timer);
    if (reconn) {
        btstack_run_loop_set_timer_handler(&s_adv_timer, adv_timer);
        btstack_run_loop_set_timer(&s_adv_timer, s_le_reconn_until - now_ms());
        btstack_run_loop_add_timer(&s_adv_timer);
    }
    if (want == s_adv_on) return;
    if (want) {
        bd_addr_t null_addr = {0};
        s_adv_data[2] = g_window == 1 ? 0x06 : 0x04; /* general discoverable only inside the window */
        gap_advertisements_set_params(0x30, 0x30, 0, 0, null_addr, 0x07, 0x00);
        gap_advertisements_set_data(sizeof s_adv_data, s_adv_data);
    }
    gap_advertisements_enable(want);
    s_adv_on = want;
    bt_logf("LE advertising %s", want ? "on" : "off");
}

bool bt_le_handle_is_host(hci_con_handle_t handle) {
    for (size_t i = 0; i < sizeof s_pending / sizeof s_pending[0]; i++)
        if (s_pending[i] == handle) return true;
    return bt_host_find_handle(handle) >= 0;
}

static void pending_set(hci_con_handle_t handle, bool on) {
    for (size_t i = 0; i < sizeof s_pending / sizeof s_pending[0]; i++) {
        if (on && s_pending[i] == 0) { s_pending[i] = handle; return; }
        if (!on && s_pending[i] == handle) s_pending[i] = 0;
    }
}

static void le_up(int s, hci_con_handle_t handle) {
    bt_host_t *h = &g_hosts[s];
    if (h->connected && h->handle == handle) return;
    make_room();
    h->connected = true;
    h->handle = handle;
    h->cid = BT_LE_CID_FLAG | handle;
    h->last_used_ms = now_ms();
    s_le_reconn_until = 0;
    bt_logf("slot %d: LE host connected", s);
    bt_hid_connected(s, true); /* keys wait until the host subscribes to the reports */
    bt_push_hosts();
}

void bt_le_host_connected(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type) {
    (void)addr_type;
    s_adv_on = false; /* the controller stops advertising on a connection */
    int s = bt_host_find_addr(addr, BT_TR_LE);
    if (s >= 0) {
        le_up(s, handle);
    } else {
        pending_set(handle, true); /* a private address or a new host: the security manager tells */
    }
    sm_request_pairing(handle); /* bonded host: makes it encrypt; new host: only inside the window */
    bt_adv_update();
}

void bt_le_host_identified(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type, bool new_bond) {
    pending_set(handle, false);
    int s = bt_host_find_addr(addr, BT_TR_LE);
    if (s < 0) {
        if (!(new_bond && g_window == 1)) { /* a bond nobody asked for */
            gap_delete_bonding((bd_addr_type_t)addr_type, (uint8_t *)addr);
            gap_disconnect(handle);
            return;
        }
        s = bt_host_alloc(BT_TR_LE, addr_type, addr, "");
        if (s < 0) {
            gap_delete_bonding((bd_addr_type_t)addr_type, (uint8_t *)addr);
            gap_disconnect(handle);
            bt_pair_finish(BT_KIND_HOST, BT_PR_FULL, BT_SLOT_NONE);
            return;
        }
        bt_host_save(s);
        le_up(s, handle);
        bt_pair_finish(BT_KIND_HOST, BT_PR_OK, s);
        return;
    }
    le_up(s, handle);
}

void bt_le_host_gone(hci_con_handle_t handle) {
    pending_set(handle, false);
    int s = bt_host_find_handle(handle);
    if (s >= 0 && g_hosts[s].transport == BT_TR_LE) {
        bt_hid_disconnected(s);
        g_hosts[s].connected = false;
        g_hosts[s].cid = 0;
        g_hosts[s].handle = HCI_CON_HANDLE_INVALID;
        bt_logf("slot %d: LE host disconnected", s);
        bt_push_hosts();
    }
    s_adv_on = false;
    bt_adv_update();
}
#else
void bt_adv_update(void) {}
#endif

/* ---- connect / forget ---- */

void bt_connect_slot(int slot) {
    if (slot < 0 || slot >= g_hosts_n) return;
    bt_host_t *h = &g_hosts[slot];
    if (!h->used || h->connected || g_bt_state != BT_ST_READY) return;
    make_room();
    if (h->transport == BT_TR_LE) {
#ifdef HH_BT_LE
        if (!g_bt_le) return;
        s_le_reconn_until = now_ms() + BT_LE_RECONNECT_MS;
        bt_adv_update();
#endif
        return;
    }
    loop_start(slot);
}

void bt_forget_host(int slot) {
    if (slot < 0 || slot >= g_hosts_n || !g_hosts[slot].used) return;
    bt_host_t *h = &g_hosts[slot];
    bt_logf("slot %d: forgotten", slot);
    rq_remove(h->addr);
    if (s_loop_slot == slot) loop_next();
    drop_conn(slot);
    bt_hid_drop_deferred(slot);
    if (h->transport == BT_TR_CLASSIC) gap_drop_link_key_for_bd_addr(h->addr);
#ifdef HH_BT_LE
    else gap_delete_bonding((bd_addr_type_t)h->addr_type, h->addr);
#endif
    bt_host_free(slot);
    bt_push_hosts();
    bt_adv_update();
}

void bt_pairing_controller_ready(void) {
#ifdef HH_BT_LE
    g_bt_le = hci_le_supported();
#else
    g_bt_le = false;
#endif
    bt_logf("controller ready, LE %s", g_bt_le ? "supported" : "not available");
}
