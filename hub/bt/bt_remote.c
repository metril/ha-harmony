#include "bt_remote.h"

#include "bt_pairing.h"

void bt_rkey_diff(bt_rkey_set_t *old, const bt_rkey_set_t *now, bt_rkey_emit_fn emit, void *ctx) {
    for (int i = 0; i < old->n; i++) {
        bool still = false;
        for (int j = 0; j < now->n; j++)
            if (now->act[j].page == old->act[i].page && now->act[j].usage == old->act[i].usage) still = true;
        if (!still) emit(old->act[i].page, old->act[i].usage, false, ctx);
    }
    for (int j = 0; j < now->n; j++) {
        bool had = false;
        for (int i = 0; i < old->n; i++)
            if (now->act[j].page == old->act[i].page && now->act[j].usage == old->act[i].usage) had = true;
        if (!had) emit(now->act[j].page, now->act[j].usage, true, ctx);
    }
    *old = *now;
}

#ifdef HH_BT_LE
#include "ad_parser.h"
#include "ble/gatt-service/hids_host.h"
#include "ble/gatt_client.h"
#include "btstack_hid_parser.h"

#define SCAN_PASSIVE_INT 96
#define SCAN_PASSIVE_WIN 24
#define CONNECT_TIMEOUT_MS 10000

static uint8_t s_hid_desc_storage[1000];

static struct {
    bool active;
    int slot; /* -1: a new remote (inside the window) */
    uint8_t addr_type;
    bd_addr_t addr;
    char name[BT_NAME_MAX + 1];
    hci_con_handle_t handle;
} s_cand;
static btstack_timer_source_t s_cand_timer;
static bool s_scanning;

static bool wants_scan(void) {
    if (!g_bt_le || g_bt_state != BT_ST_READY || g_classic_paging || s_cand.active) return false;
    if (g_window == 2) return true;
    for (int i = 0; i < g_remotes_n; i++)
        if (g_remotes[i].used && !g_remotes[i].connected) return true;
    return false;
}

void bt_remote_scan_update(void) {
    bool want = wants_scan();
    if (want) {
        /* the window scans actively (names come in scan responses); waiting for a bonded remote
         * is passive at 25% duty so Classic paging keeps its share of the radio */
        if (g_window == 2) gap_set_scan_parameters(1, 48, 48);
        else gap_set_scan_parameters(0, SCAN_PASSIVE_INT, SCAN_PASSIVE_WIN);
    }
    if (want == s_scanning && !want) return;
    if (want) gap_start_scan();
    else if (s_scanning) gap_stop_scan();
    s_scanning = want;
}

void bt_remote_init(void) {
    gatt_client_init();
    hids_host_init(s_hid_desc_storage, sizeof s_hid_desc_storage);
    for (int i = 0; i < g_remotes_n; i++) g_remotes[i].handle = HCI_CON_HANDLE_INVALID;
}

static void cand_clear(void) {
    btstack_run_loop_remove_timer(&s_cand_timer);
    s_cand.active = false;
    bt_remote_scan_update();
}

static void cand_timeout(btstack_timer_source_t *ts) {
    (void)ts;
    log_msg("remote: connection attempt timed out");
    gap_connect_cancel();
    cand_clear();
}

static void start_connect(int slot, const bd_addr_t addr, uint8_t addr_type, const char *name) {
    memset(&s_cand, 0, sizeof s_cand);
    s_cand.active = true;
    s_cand.slot = slot;
    s_cand.addr_type = addr_type;
    memcpy(s_cand.addr, addr, 6);
    s_cand.handle = HCI_CON_HANDLE_INVALID;
    snprintf(s_cand.name, sizeof s_cand.name, "%s", name);
    if (s_scanning) gap_stop_scan();
    s_scanning = false;
    bt_logf("remote: connecting to %s%s", bd_addr_to_str(addr), slot < 0 ? " (new)" : "");
    gap_connect(s_cand.addr, (bd_addr_type_t)addr_type);
    btstack_run_loop_set_timer_handler(&s_cand_timer, cand_timeout);
    btstack_run_loop_set_timer(&s_cand_timer, CONNECT_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_cand_timer);
}

void bt_remote_adv_report(const uint8_t *packet) {
    if (!s_scanning || s_cand.active) return;
    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    uint8_t type = gap_event_advertising_report_get_address_type(packet);
    const uint8_t *ad = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);

    int slot = bt_remote_find_addr(addr);
    if (slot >= 0) {
        if (!g_remotes[slot].connected) start_connect(slot, addr, g_remotes[slot].addr_type, g_remotes[slot].name);
        return;
    }
    if (g_window != 2) return;
    char name[BT_NAME_MAX + 1] = "";
    uint16_t appearance = 0;
    ad_context_t ctx;
    for (ad_iterator_init(&ctx, ad_len, ad); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
        uint8_t t = ad_iterator_get_data_type(&ctx), l = ad_iterator_get_data_len(&ctx);
        const uint8_t *d = ad_iterator_get_data(&ctx);
        if ((t == 0x08 || t == 0x09) && l) {
            uint8_t n = l < BT_NAME_MAX ? l : BT_NAME_MAX;
            memcpy(name, d, n);
            name[n] = 0;
        } else if (t == 0x19 && l >= 2) {
            appearance = (uint16_t)(d[0] | (d[1] << 8));
        }
    }
    bool hid = ad_data_contains_uuid16(ad_len, ad, 0x1812) || (appearance >= 0x03C0 && appearance <= 0x03C4);
    if (hid) start_connect(-1, addr, type, name);
}

bool bt_remote_owns_handle(hci_con_handle_t handle) {
    if (s_cand.active && s_cand.handle == handle) return true;
    for (int i = 0; i < g_remotes_n; i++)
        if (g_remotes[i].used && g_remotes[i].connected && g_remotes[i].handle == handle) return true;
    return false;
}

bool bt_remote_le_connected(hci_con_handle_t handle) {
    if (!s_cand.active) return false;
    btstack_run_loop_remove_timer(&s_cand_timer);
    s_cand.handle = handle;
    sm_request_pairing(handle); /* new remote: pairing (confirmed in the window); bonded: re-encryption */
    return true;
}

void bt_remote_le_connect_failed(void) {
    if (s_cand.active) cand_clear();
}

void bt_remote_just_works(hci_con_handle_t handle) {
    if (g_window == 2 && s_cand.active && s_cand.slot < 0) sm_just_works_confirm(handle);
    else sm_bonding_decline(handle);
}

static void report_key(uint8_t page, uint16_t usage, bool down, void *ctx) {
    bt_push_rkey((int)(intptr_t)ctx, page, usage, down);
}

static void release_all(int slot) {
    bt_rkey_set_t none = {.n = 0};
    bt_rkey_diff(&g_remotes[slot].active, &none, report_key, (void *)(intptr_t)slot);
}

static void hids_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
        uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
        int slot = s_cand.active ? s_cand.slot : -1;
        for (int i = 0; i < g_remotes_n; i++) if (g_remotes[i].used && g_remotes[i].hids_cid == cid) slot = i;
        if (gattservice_subevent_hid_service_connected_get_status(packet) != ERROR_CODE_SUCCESS || slot < 0) {
            log_msg("remote: HIDS connect failed");
            if (slot >= 0 && g_remotes[slot].handle != HCI_CON_HANDLE_INVALID) gap_disconnect(g_remotes[slot].handle);
            return;
        }
        g_remotes[slot].connected = true;
        bt_logf("remote %d: connected", slot);
        if (s_cand.active) cand_clear();
        bt_push_remotes();
        break;
    }
    case GATTSERVICE_SUBEVENT_HID_REPORT: {
        uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
        uint8_t svc = gattservice_subevent_hid_report_get_service_index(packet);
        int slot = -1;
        for (int i = 0; i < g_remotes_n; i++) if (g_remotes[i].used && g_remotes[i].connected && g_remotes[i].hids_cid == cid) slot = i;
        if (slot < 0) return;
        btstack_hid_parser_t p;
        btstack_hid_parser_init(&p, hids_host_descriptor_storage_get_descriptor_data(cid, svc),
                                hids_host_descriptor_storage_get_descriptor_len(cid, svc), HID_REPORT_TYPE_INPUT,
                                gattservice_subevent_hid_report_get_report(packet),
                                gattservice_subevent_hid_report_get_report_len(packet));
        bt_rkey_set_t now = {.n = 0};
        while (btstack_hid_parser_has_more(&p)) {
            uint16_t page, usage;
            int32_t value;
            btstack_hid_parser_get_field(&p, &page, &usage, &value);
            if ((page != 0x07 && page != 0x0C) || value == 0 || usage == 0 || now.n >= BT_RKEY_MAX) continue;
            now.act[now.n++] = (bt_rkey_t){(uint8_t)page, usage};
        }
        bt_rkey_diff(&g_remotes[slot].active, &now, report_key, (void *)(intptr_t)slot);
        break;
    }
    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED: {
        uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
        for (int i = 0; i < g_remotes_n; i++)
            if (g_remotes[i].used && g_remotes[i].hids_cid == cid) bt_remote_gone(g_remotes[i].handle);
        break;
    }
    default:
        break;
    }
}

static void hids_start(int slot, hci_con_handle_t handle) {
    g_remotes[slot].handle = handle;
    uint16_t cid = 0;
    uint8_t st = hids_host_connect(handle, hids_event, HID_PROTOCOL_MODE_REPORT, &cid);
    g_remotes[slot].hids_cid = cid;
    if (st != ERROR_CODE_SUCCESS) {
        bt_logf("remote %d: hids_host_connect 0x%02x", slot, st);
        gap_disconnect(handle);
    }
}

void bt_remote_pairing_complete(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type, uint8_t status) {
    if (!s_cand.active || s_cand.handle != handle) return;
    if (status != ERROR_CODE_SUCCESS) {
        bt_logf("remote: pairing failed (0x%02x)", status);
        gap_disconnect(handle);
        if (g_window == 2) bt_push_pairres(BT_KIND_REMOTE, BT_PR_FAILED, BT_SLOT_NONE);
        return;
    }
    int slot = s_cand.slot;
    if (slot < 0) {
        slot = bt_remote_alloc(addr_type, addr, s_cand.name);
        if (slot < 0) {
            gap_delete_bonding((bd_addr_type_t)addr_type, (uint8_t *)addr);
            gap_disconnect(handle);
            bt_pair_finish(BT_KIND_REMOTE, BT_PR_FULL, BT_SLOT_NONE);
            return;
        }
        bt_remote_save(slot);
        s_cand.slot = slot;
        bt_pair_finish(BT_KIND_REMOTE, BT_PR_OK, slot);
    }
    gap_request_connection_parameter_update(handle, 6, 12, 0, 200);
    hids_start(slot, handle);
    bt_push_remotes();
}

void bt_remote_reencrypted(hci_con_handle_t handle, uint8_t status) {
    if (!s_cand.active || s_cand.handle != handle || s_cand.slot < 0) return;
    if (status != ERROR_CODE_SUCCESS) {
        /* the bond stays: only a window (not an advertiser claiming the address) replaces it */
        bt_logf("remote %d: re-encryption failed (0x%02x)", s_cand.slot, status);
        gap_disconnect(handle);
        return;
    }
    gap_request_connection_parameter_update(handle, 6, 12, 0, 200);
    hids_start(s_cand.slot, handle);
}

void bt_remote_gone(hci_con_handle_t handle) {
    if (s_cand.active && s_cand.handle == handle) cand_clear();
    for (int i = 0; i < g_remotes_n; i++) {
        bt_remote_t *r = &g_remotes[i];
        if (!r->used || r->handle != handle) continue;
        release_all(i);
        bool was = r->connected;
        r->connected = false;
        r->handle = HCI_CON_HANDLE_INVALID;
        r->hids_cid = 0;
        if (was) bt_logf("remote %d: disconnected", i);
        bt_push_remotes();
    }
    bt_remote_scan_update();
}

void bt_remote_forget(int slot) {
    if (slot < 0 || slot >= g_remotes_n || !g_remotes[slot].used) return;
    bt_remote_t *r = &g_remotes[slot];
    bt_logf("remote %d: forgotten", slot);
    if (s_cand.active && s_cand.slot == slot) { gap_connect_cancel(); cand_clear(); }
    release_all(slot);
    if (r->handle != HCI_CON_HANDLE_INVALID) gap_disconnect(r->handle);
    gap_delete_bonding((bd_addr_type_t)r->addr_type, r->addr);
    bt_remote_free(slot);
    bt_push_remotes();
    bt_remote_scan_update();
}
#else
void bt_remote_init(void) {}
void bt_remote_scan_update(void) {}
void bt_remote_forget(int slot) { (void)slot; }
void bt_remote_adv_report(const uint8_t *packet) { (void)packet; }
bool bt_remote_owns_handle(hci_con_handle_t handle) { (void)handle; return false; }
bool bt_remote_le_connected(hci_con_handle_t handle) { (void)handle; return false; }
void bt_remote_le_connect_failed(void) {}
void bt_remote_just_works(hci_con_handle_t handle) { (void)handle; }
void bt_remote_pairing_complete(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type, uint8_t status) {
    (void)handle; (void)addr; (void)addr_type; (void)status;
}
void bt_remote_reencrypted(hci_con_handle_t handle, uint8_t status) { (void)handle; (void)status; }
void bt_remote_gone(hci_con_handle_t handle) { (void)handle; }
#endif
