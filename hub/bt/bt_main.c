/* `hub bt-helper [--keys PATH] [--hci N] [--fd N]`: Bluetooth HID helper. Classic HID device,
 * LE HID peripheral and LE HOGP host (HH_BT_LE) around BTstack, controlled by the parent over an
 * inherited socketpair fd (bt_ipc.h). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt_bonds.h"
#include "bt_hid.h"
#include "bt_main.h"
#include "bt_txq.h"
#include "bt_pairing.h"
#include "bt_remote.h"
#include "bt_win.h"
#include "bt_leaddr.h"
#include "bt_sc.h"
#include "bt_trace.h"
#include "bt_hid_queue.h"
#include "btstack_memory.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "hci_transport_linux.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/device_id_server.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#ifdef HH_BT_LE
#include "ble/gatt-service/battery_service_server.h"
#include "ble/gatt-service/device_information_service_server.h"
#include "ble/gatt-service/hids_device.h"
#include "ble/le_device_db_tlv.h"
#include "hh_bt_gatt.h"
#endif

#define BT_NAME "Harmony Hub"

/* ---- IPC ---- */

#define TX_CAP 65536 /* parent stuck beyond this: exit instead of dropping frames */

static int s_fd = -1;
static uint8_t s_rx[2 * (BT_IPC_MAX_PAYLOAD + BT_IPC_HDR)];
static size_t s_rx_n;
static uint8_t s_tx[TX_CAP];
static btstack_data_source_t s_ipc_ds;
static bool s_ipc_registered;
static volatile sig_atomic_t s_stop;
static int s_exit_code;

static void ipc_stop(void) {
    s_stop = 1;
    btstack_run_loop_trigger_exit();
}

static bt_txq_t s_txq = {s_tx, 0, TX_CAP};

/* writes what the socket takes; false when the peer is gone */
static bool tx_drain(void) {
    if (bt_txq_drain(&s_txq, s_fd) < 0) return false;
    if (s_ipc_registered) {
        if (s_txq.n) btstack_run_loop_enable_data_source_callbacks(&s_ipc_ds, DATA_SOURCE_CALLBACK_WRITE);
        else btstack_run_loop_disable_data_source_callbacks(&s_ipc_ds, DATA_SOURCE_CALLBACK_WRITE);
    }
    return true;
}

/* frames are queued whole and in order; nothing is dropped or split */
static void ipc_send(uint8_t type, const uint8_t *p, size_t n) {
    if (s_fd < 0 || s_stop) return;
    uint8_t buf[BT_IPC_HDR + BT_IPC_MAX_PAYLOAD];
    size_t len = bt_ipc_frame(buf, type, p, n);
    if (!len) return;
    if (bt_txq_push(&s_txq, buf, len) < 0) {
        log_msg("ipc: parent is not reading, exiting");
        s_exit_code = 1;
        ipc_stop();
        return;
    }
    if (!tx_drain()) {
        log_msg("ipc: write failed, exiting");
        s_exit_code = 1;
        ipc_stop();
    }
}

/* before exit: give the parent a moment to take what is queued */
static void ipc_flush_blocking(void) {
    for (int i = 0; i < 100 && s_txq.n && s_fd >= 0; i++) {
        if (!tx_drain()) return;
        if (s_txq.n) usleep(10000);
    }
}

void bt_push_state(void) {
    uint8_t p[6] = {g_bt_state, g_window, g_bt_le, HH_BT_MAX_HOSTS, HH_BT_MAX_CONN, HH_BT_MAX_REMOTES};
    ipc_send(BT_IPC_STATE, p, sizeof p);
}

static size_t put_name(uint8_t *out, const char *name) {
    size_t n = strnlen(name, BT_NAME_MAX);
    out[0] = (uint8_t)n;
    memcpy(out + 1, name, n);
    return n + 1;
}

/* chunked list frames: flags (bit 0 = first chunk), n, entries */
typedef struct {
    uint8_t type, n;
    bool first;
    size_t o;
    uint8_t buf[BT_IPC_MAX_PAYLOAD];
} list_t;

static void list_begin(list_t *l, uint8_t type) {
    l->type = type;
    l->n = 0;
    l->first = true;
    l->o = 2;
}

static void list_flush(list_t *l) {
    l->buf[0] = l->first;
    l->buf[1] = l->n;
    ipc_send(l->type, l->buf, l->o);
    l->first = false;
    l->n = 0;
    l->o = 2;
}

static uint8_t *list_room(list_t *l) {
    if (l->n == BT_IPC_LIST_CHUNK) list_flush(l);
    return l->buf + l->o;
}

static void list_added(list_t *l, size_t len) {
    l->o += len;
    l->n++;
}

static void list_end(list_t *l) {
    if (l->n || l->first) list_flush(l);
}

void bt_push_hosts(void) {
    static list_t l;
    list_begin(&l, BT_IPC_HOSTS);
    for (int i = 0; i < g_hosts_n; i++) {
        const bt_host_t *h = &g_hosts[i];
        if (!h->used) continue;
        uint8_t *p = list_room(&l);
        size_t o = 0;
        p[o++] = (uint8_t)i;
        p[o++] = h->transport;
        p[o++] = h->addr_type;
        p[o++] = h->connected;
        memcpy(p + o, h->addr, 6);
        o += 6;
        o += put_name(p + o, h->name);
        list_added(&l, o);
    }
    list_end(&l);
}

void bt_push_remotes(void) {
    static list_t l;
    list_begin(&l, BT_IPC_REMOTES);
    for (int i = 0; i < g_remotes_n; i++) {
        const bt_remote_t *r = &g_remotes[i];
        if (!r->used || r->pending) continue;
        uint8_t *p = list_room(&l);
        size_t o = 0;
        p[o++] = (uint8_t)i;
        p[o++] = r->addr_type;
        p[o++] = r->connected;
        p[o++] = BT_BATTERY_UNKNOWN;
        p[o++] = r->fails;
        memcpy(p + o, r->addr, 6);
        o += 6;
        o += put_name(p + o, r->name);
        list_added(&l, o);
    }
    list_end(&l);
}

void bt_push_candidate(const bd_addr_t addr, uint8_t addr_type, int8_t rssi, const char *name) {
    uint8_t p[6 + 3 + 1 + BT_NAME_MAX];
    memcpy(p, addr, 6);
    p[6] = addr_type;
    p[7] = (uint8_t)rssi;
    size_t n = 8 + put_name(p + 8, name);
    ipc_send(BT_IPC_CANDIDATE, p, n);
}

void bt_push_pairres(uint8_t kind, uint8_t result, int slot) {
    uint8_t p[3 + 6 + 2 + BT_NAME_MAX] = {kind, result, BT_SLOT_NONE};
    uint8_t tr = BT_TR_CLASSIC;
    const char *name = "";
    if (kind == BT_KIND_HOST && slot >= 0 && slot < g_hosts_n && g_hosts[slot].used) {
        p[2] = (uint8_t)slot;
        memcpy(p + 3, g_hosts[slot].addr, 6);
        tr = g_hosts[slot].transport;
        name = g_hosts[slot].name;
    } else if (kind == BT_KIND_REMOTE && slot >= 0 && slot < g_remotes_n && g_remotes[slot].used) {
        p[2] = (uint8_t)slot;
        memcpy(p + 3, g_remotes[slot].addr, 6);
        tr = BT_TR_LE;
        name = g_remotes[slot].name;
    }
    p[9] = tr;
    size_t n = 10 + put_name(p + 10, name);
    ipc_send(BT_IPC_PAIRRES, p, n);
}

void bt_push_rkey(int slot, uint8_t page, uint16_t usage, bool down) {
    uint8_t p[5] = {(uint8_t)slot, page, (uint8_t)(usage >> 8), (uint8_t)usage, down};
    ipc_send(BT_IPC_RKEY, p, sizeof p);
}

static void push_all(void) {
    bt_push_state();
    bt_push_hosts();
    bt_push_remotes();
}

/* the transport hit a fatal read error: report it and leave (never spin) */
void bt_transport_fatal(int err) {
    g_bt_state = (err == ENODEV || err == ENXIO || err == ENETDOWN) ? BT_ST_NO_ADAPTER : BT_ST_ERROR;
    bt_logf("HCI transport failed (errno %d)", err);
    bt_push_state();
    s_exit_code = 1;
    ipc_stop();
}

static void ipc_handle(uint8_t type, const uint8_t *p, size_t n) {
    switch (type) {
    case BT_IPC_PING:
        if (n >= 1) ipc_send(BT_IPC_PONG, p, 1);
        break;
    case BT_IPC_KEY:
        if (n >= 5) bt_hid_key(p[0], p[1], (uint16_t)((p[2] << 8) | p[3]), p[4] != 0);
        break;
    case BT_IPC_PAIR:
        if (n >= 3)
            bt_pair_window(p[0] != 0, (uint16_t)((p[1] << 8) | p[2]),
                           n >= 4 && p[3] == BT_KIND_REMOTE ? BT_KIND_REMOTE : BT_KIND_HOST,
                           n >= 5 ? p[4] : BT_PAIR_TR_ANY);
        break;
    case BT_IPC_FORGET:
        if (n >= 2) {
            if (p[0] == BT_KIND_REMOTE) bt_remote_forget(p[1]);
            else bt_forget_host(p[1]);
        }
        break;
    case BT_IPC_CONNECT:
        if (n >= 1) bt_connect_slot(p[0]);
        break;
    case BT_IPC_GET:
        push_all();
        break;
    case BT_IPC_PAIR_REMOTE:
        if (n >= 7) bt_remote_pair(p, p[6]);
        break;
    default:
        break;
    }
}

static void ipc_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
    (void)ds;
    if (type == DATA_SOURCE_CALLBACK_WRITE) {
        if (!tx_drain()) {
            log_msg("ipc: write failed, exiting");
            ipc_stop();
        }
        return;
    }
    if (type != DATA_SOURCE_CALLBACK_READ) return;
    ssize_t r = read(s_fd, s_rx + s_rx_n, sizeof s_rx - s_rx_n);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
        log_msg("parent closed the control channel: exiting");
        ipc_stop();
        return;
    }
    if (r > 0) s_rx_n += (size_t)r;
    size_t off = 0;
    for (;;) {
        uint8_t t;
        const uint8_t *pl;
        size_t pn;
        size_t used = bt_ipc_parse(s_rx + off, s_rx_n - off, &t, &pl, &pn);
        if (used == (size_t)-1) {
            log_msg("ipc: bad frame, exiting");
            ipc_stop();
            return;
        }
        if (!used) break;
        ipc_handle(t, pl, pn);
        off += used;
    }
    memmove(s_rx, s_rx + off, s_rx_n - off);
    s_rx_n -= off;
}

/* ---- BTstack events ---- */

static btstack_packet_callback_registration_t s_hci_cb;
#ifdef HH_BT_LE
static uint8_t s_le_addr[6];
static bool s_le_addr_ok; /* the persisted random static LE address is ready to use */
#endif
#ifdef HH_BT_LE
static btstack_packet_callback_registration_t s_sm_cb;
#endif

static void on_working(void) {
    gap_set_local_name(BT_NAME);
    gap_set_class_of_device(0x002540); /* peripheral, keyboard */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_security_mode(GAP_SECURITY_MODE_2);
    gap_connectable_control(1);
    gap_discoverable_control(0);
    hci_disable_l2cap_timeout_check();
    bt_pairing_controller_ready();
#ifdef HH_BT_LE
    if (g_bt_le && s_le_addr_ok) { /* LE uses its own static random address, not the public BD_ADDR Classic hosts know */
        gap_random_address_set(s_le_addr);
        bt_logf("LE address (static random) %s", bd_addr_to_str(s_le_addr));
    }
    if (g_bt_le) gap_set_connection_parameters(48, 48, 6, 12, 0, 200, 0, 0); /* fast central connects */
#endif
    g_bt_state = BT_ST_READY;
    push_all();
    bt_remote_scan_update();
}

/* one line per pairing failure (default log level): transport, peer, the stack's reason; never key material */
static void log_pair_fail(const char *transport, const bd_addr_t addr, const char *what, uint8_t code) {
    char line[120];
    bt_pair_fail_line(line, sizeof line, transport, bd_addr_to_str(addr), what, code);
    log_msg(line);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) on_working();
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t a;
        hci_event_pin_code_request_get_bd_addr(packet, a);
        bool ok = bt_pair_classic_open();
        if (ok) bt_logf("pairing start: transport classic, peer %s, PIN request", bd_addr_to_str(a));
        else bt_logf("pairing refused: transport classic, peer %s, PIN request outside a classic window", bd_addr_to_str(a));
        if (ok) gap_pin_code_response(a, "0000");
        else gap_pin_code_negative(a);
        break;
    }
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t a;
        hci_event_user_confirmation_request_get_bd_addr(packet, a);
        bool ok = bt_pair_classic_open();
        if (!ok) bt_logf("pairing refused: transport classic, peer %s, confirmation outside a classic window", bd_addr_to_str(a));
        if (ok) gap_ssp_confirmation_response(a);
        else gap_ssp_confirmation_negative(a);
        break;
    }
    case HCI_EVENT_USER_PASSKEY_REQUEST: {
        bd_addr_t a;
        hci_event_user_passkey_request_get_bd_addr(packet, a);
        bool ok = bt_pair_classic_open();
        if (!ok) bt_logf("pairing refused: transport classic, peer %s, passkey outside a classic window", bd_addr_to_str(a));
        if (ok) gap_ssp_passkey_response(a, 0);
        else gap_ssp_passkey_negative(a);
        break;
    }
    case HCI_EVENT_LINK_KEY_NOTIFICATION: {
        bd_addr_t a;
        reverse_bd_addr(&packet[2], a);
        bt_classic_link_key(a);
        break;
    }
    case HCI_EVENT_COMMAND_COMPLETE: {
        uint16_t op = hci_event_command_complete_get_command_opcode(packet);
        const char *nm = op == hci_le_set_advertising_parameters.opcode ? "LE Set Advertising Parameters"
                       : op == hci_le_set_advertising_data.opcode ? "LE Set Advertising Data"
                       : op == hci_le_set_advertise_enable.opcode ? "LE Set Advertise Enable" : NULL;
        if (!nm) break;
        static uint8_t okseen;
        uint8_t bit = op == hci_le_set_advertising_parameters.opcode ? 1 : op == hci_le_set_advertising_data.opcode ? 2 : 4;
        uint8_t st = hci_event_command_complete_get_return_parameters(packet)[0];
        if (st != ERROR_CODE_SUCCESS) bt_logf("%s failed: HCI status 0x%02x", nm, st);
        else if (!(okseen & bit)) { okseen |= bit; bt_logf("%s: ok (first time)", nm); }
        break;
    }
    case HCI_EVENT_CONNECTION_REQUEST: {
        bd_addr_t a;
        hci_event_connection_request_get_bd_addr(packet, a);
        bt_logf("connection request: peer %s, link type %u (ACL=1), class 0x%06x, classic window %s, page scan decides", bd_addr_to_str(a),
                (unsigned)hci_event_connection_request_get_link_type(packet), (unsigned)hci_event_connection_request_get_class_of_device(packet),
                bt_pair_classic_open() ? "open" : "closed");
        break;
    }
    case HCI_EVENT_CONNECTION_COMPLETE: {
        bd_addr_t a;
        hci_event_connection_complete_get_bd_addr(packet, a);
        uint8_t st = hci_event_connection_complete_get_status(packet);
        bt_logf("classic connection %s: peer %s, HCI status 0x%02x", st == ERROR_CODE_SUCCESS ? "up" : "failed", bd_addr_to_str(a), st);
        break;
    }
    case HCI_EVENT_IO_CAPABILITY_REQUEST: {
        bd_addr_t a;
        hci_event_io_capability_request_get_bd_addr(packet, a);
        if (bt_pair_classic_open()) bt_logf("pairing start: transport classic, peer %s, SSP", bd_addr_to_str(a));
        else { /* a bonded host reconnecting re-authenticates: routine */
            char l[96];
            snprintf(l, sizeof l, "authentication: transport classic, peer %s, SSP (no window)", bd_addr_to_str(a));
            bt_dbg(l);
        }
        break;
    }
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
        uint8_t st = hci_event_simple_pairing_complete_get_status(packet);
        if (st == ERROR_CODE_SUCCESS) break;
        bd_addr_t a;
        hci_event_simple_pairing_complete_get_bd_addr(packet, a);
        log_pair_fail("classic", a, "HCI status", st);
        if (bt_pair_classic_open()) bt_push_pairres(BT_KIND_HOST, BT_PR_FAILED, -1);
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE: {
        uint8_t st = hci_event_authentication_complete_get_status(packet);
        if (st == ERROR_CODE_SUCCESS) break;
        hci_connection_t *c = hci_connection_for_handle(hci_event_authentication_complete_get_connection_handle(packet));
        if (c && bt_pair_classic_open()) log_pair_fail("classic", c->address, "auth status", st);
        else if (c) { /* reconnect retries of a bonded host: debug only */
            char l[120];
            bt_pair_fail_line(l, sizeof l, "classic", bd_addr_to_str(c->address), "auth status", st);
            bt_dbg(l);
        }
        break;
    }
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
        if (hci_event_remote_name_request_complete_get_status(packet) != ERROR_CODE_SUCCESS) break;
        bd_addr_t a;
        char name[BT_NAME_MAX + 1] = "";
        hci_event_remote_name_request_complete_get_bd_addr(packet, a);
        size_t room = (size_t)packet[1] + 2 > 9 ? (size_t)packet[1] + 2 - 9 : 0;
        const uint8_t *raw = packet + 9;
        for (size_t i = 0; i < room && i < BT_NAME_MAX && raw[i]; i++) name[i] = raw[i] >= 0x20 && raw[i] < 0x7f ? (char)raw[i] : '?';
        bt_classic_name(a, name);
        break;
    }
    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED: {
            bd_addr_t a;
            hid_subevent_connection_opened_get_bd_addr(packet, a);
            bt_classic_hid_opened(hid_subevent_connection_opened_get_status(packet),
                                  hid_subevent_connection_opened_get_hid_cid(packet), a,
                                  hid_subevent_connection_opened_get_con_handle(packet));
            break;
        }
        case HID_SUBEVENT_CONNECTION_CLOSED:
            bt_classic_hid_closed(hid_subevent_connection_closed_get_hid_cid(packet));
            break;
        case HID_SUBEVENT_CAN_SEND_NOW:
            bt_hid_can_send_now(hid_subevent_can_send_now_get_hid_cid(packet));
            break;
        default:
            break;
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        hci_con_handle_t h = hci_event_disconnection_complete_get_connection_handle(packet);
        {
            int hs = bt_host_find_handle(h);
            bt_logf("link 0x%04x (%s) disconnected: status 0x%02x, reason 0x%02x", (unsigned)h,
                    hs < 0 ? "not a host" : g_hosts[hs].transport == BT_TR_LE ? "le host" : "classic host",
                    (unsigned)hci_event_disconnection_complete_get_status(packet), (unsigned)hci_event_disconnection_complete_get_reason(packet));
        }
        bt_classic_acl_gone(h);
#ifdef HH_BT_LE
        bt_le_host_gone(h);
        bt_remote_gone(h);
        bt_sc_drop(h);
#endif
        break;
    }
#ifdef HH_BT_LE
    case GAP_EVENT_ADVERTISING_REPORT:
        bt_remote_adv_report(packet);
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            hci_con_handle_t h = gap_subevent_le_connection_complete_get_connection_handle(packet);
            bool central = gap_subevent_le_connection_complete_get_role(packet) == 0;
            {
                bd_addr_t pa;
                gap_subevent_le_connection_complete_get_peer_address(packet, pa);
                bt_logf("LE connection %s: role %s, peer %s (type %u), HCI status 0x%02x, handle 0x%04x",
                        gap_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS ? "up" : "failed",
                        central ? "central" : "peripheral", bd_addr_to_str(pa),
                        (unsigned)gap_subevent_le_connection_complete_get_peer_address_type(packet),
                        (unsigned)gap_subevent_le_connection_complete_get_status(packet), (unsigned)h);
            }
            if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                if (central) bt_remote_le_connect_failed();
                break;
            }
            if (central) {
                if (!bt_remote_le_connected(h)) gap_disconnect(h); /* an attempt that was given up */
            } else {
                bd_addr_t a;
                gap_subevent_le_connection_complete_get_peer_address(packet, a);
                bt_le_host_connected(h, a, gap_subevent_le_connection_complete_get_peer_address_type(packet));
            }
        }
        break;
#endif
    default:
        break;
    }
}

#ifdef HH_BT_LE
static void sm_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t a;
    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_PAIRING_STARTED:
        { /* the event's address is still unknown (zeros): the connection's own peer address is the one to show */
            hci_con_handle_t ph = sm_event_pairing_started_get_handle(packet);
            hci_connection_t *pc = hci_connection_for_handle(ph);
            if (pc) bt_logf("pairing start: transport le, peer %s (type %u)", bd_addr_to_str(pc->address), (unsigned)pc->address_type);
            else bt_logf("pairing start: transport le, handle 0x%04x", (unsigned)ph);
        }
        break;
    case SM_EVENT_JUST_WORKS_REQUEST: {
        hci_con_handle_t h = sm_event_just_works_request_get_handle(packet);
        if (bt_remote_owns_handle(h)) bt_remote_just_works(h);
        else if (bt_pair_le_open()) sm_just_works_confirm(h);
        else {
            bt_logf("pairing refused: transport le, handle 0x%04x, no le window open", (unsigned)h);
            sm_bonding_decline(h); /* pairing only inside the window */
        }
        break;
    }
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
        hci_con_handle_t h = sm_event_numeric_comparison_request_get_handle(packet);
        if (!bt_remote_owns_handle(h) && bt_pair_le_open()) sm_numeric_comparison_confirm(h);
        else {
            bt_logf("pairing refused: transport le, handle 0x%04x, numeric comparison outside an le window", (unsigned)h);
            sm_bonding_decline(h);
        }
        break;
    }
    case SM_EVENT_PAIRING_COMPLETE: {
        hci_con_handle_t h = sm_event_pairing_complete_get_handle(packet);
        uint8_t st = sm_event_pairing_complete_get_status(packet);
        sm_event_pairing_complete_get_address(packet, a);
        uint8_t at = sm_event_pairing_complete_get_addr_type(packet);
        if (st == ERROR_CODE_SUCCESS) bt_logf("pairing complete: transport le, peer %s, ok", bd_addr_to_str(a));
        else if (st == ERROR_CODE_AUTHENTICATION_FAILURE) log_pair_fail("le", a, "SMP reason", sm_event_pairing_complete_get_reason(packet));
        else log_pair_fail("le", a, "HCI status", st);
        if (bt_remote_owns_handle(h)) bt_remote_pairing_complete(h, a, at, st);
        else if (st == ERROR_CODE_SUCCESS) bt_le_host_identified(h, a, at, true);
        else if (bt_pair_le_open()) bt_push_pairres(BT_KIND_HOST, BT_PR_FAILED, -1);
        break;
    }
    case SM_EVENT_REENCRYPTION_COMPLETE: {
        hci_con_handle_t h = sm_event_reencryption_complete_get_handle(packet);
        uint8_t st = sm_event_reencryption_complete_get_status(packet);
        sm_event_reencryption_complete_get_address(packet, a);
        if (bt_remote_owns_handle(h)) bt_remote_reencrypted(h, st);
        else if (st == ERROR_CODE_SUCCESS) bt_le_host_identified(h, a, sm_event_reencryption_complete_get_addr_type(packet), false);
        else gap_disconnect(h);
        break;
    }
    default:
        break;
    }
}

static void hids_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_HIDS_META) return;
    switch (hci_event_hids_meta_get_subevent_code(packet)) {
    case HIDS_SUBEVENT_CAN_SEND_NOW:
        bt_hid_can_send_now(BT_LE_CID_FLAG | hids_subevent_can_send_now_get_con_handle(packet));
        break;
    case HIDS_SUBEVENT_INPUT_REPORT_ENABLE: {
        int s = bt_host_find_handle(hids_subevent_input_report_enable_get_con_handle(packet));
        if (hids_subevent_input_report_enable_get_enable(packet)) {
            if (s >= 0) bt_hid_ready(s);
            else bt_hid_ccc_early(hids_subevent_input_report_enable_get_con_handle(packet)); /* applied when the host comes up */
        }
        bt_logf("HIDS input report notifications %s for handle 0x%04x (CCC written or restored)",
                hids_subevent_input_report_enable_get_enable(packet) ? "enabled" : "disabled",
                (unsigned)hids_subevent_input_report_enable_get_con_handle(packet));
        break;
    }
    default:
        break;
    }
}
static hids_device_report_t s_hids_reports[3];

/* ---- Generic Attribute service (0x1801): Service Changed ---- */
static att_service_handler_t s_gatt_svc;
static btstack_context_callback_registration_t s_sc_cb[BT_SC_SLOTS]; /* one per connection slot: a registration is queued by address */

static int sc_get(uint32_t tag, uint8_t *v, uint32_t cap) { return g_tlv ? g_tlv->get_tag(&g_tlv_ctx, tag, v, cap) : 0; }
static int sc_put(uint32_t tag, const uint8_t *v, uint32_t len) { return bt_tlv_store_if_changed(tag, v, len); }
static void sc_del(uint32_t tag) { if (g_tlv) g_tlv->delete_tag(&g_tlv_ctx, tag); }

static void sc_send(void *ctx) {
    hci_con_handle_t h = (hci_con_handle_t)(uintptr_t)ctx;
    int slot = bt_sc_slot(h);
    uint8_t v[4];
    bt_sc_value(v);
    uint8_t rc = att_server_indicate(h, ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_VALUE_HANDLE, v, sizeof v);
    if (rc == ERROR_CODE_SUCCESS) {
        bt_sc_sent(h);
        bt_logf("service changed indicated to handle 0x%04x (0x0001-0xFFFF)", (unsigned)h);
    } else if (rc != ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER && bt_sc_ccc_get(h)) { /* busy: ask again */
        if (slot < 0) return;
        s_sc_cb[slot].callback = sc_send;
        s_sc_cb[slot].context = ctx;
        att_server_register_can_send_now_callback(&s_sc_cb[slot], h);
    }
}

/* A bonded peer's connection is up and identified, or its CCCD arrived: send Service Changed (deferred so a
 * pending write response goes first). */
void bt_sc_request(uint16_t con) {
    int slot = bt_sc_slot(con);
    if (slot < 0) return;
    s_sc_cb[slot].callback = sc_send;
    s_sc_cb[slot].context = (void *)(uintptr_t)con;
    att_server_register_can_send_now_callback(&s_sc_cb[slot], con);
}

static void att_pkt(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE) return;
    if (att_event_handle_value_indication_complete_get_attribute_handle(packet) != ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_VALUE_HANDLE) return;
    hci_con_handle_t h = att_event_handle_value_indication_complete_get_conn_handle(packet);
    if (att_event_handle_value_indication_complete_get_status(packet) == ATT_ERROR_SUCCESS) {
        bt_sc_confirmed(h);
        bt_logf("service changed confirmed by handle 0x%04x; hash stored for the bond", (unsigned)h);
    } else {
        bt_logf("service changed not confirmed by handle 0x%04x (status 0x%02x)", (unsigned)h,
                (unsigned)att_event_handle_value_indication_complete_get_status(packet));
    }
}

static uint16_t gatt_read(hci_con_handle_t con, uint16_t handle, uint16_t off, uint8_t *buf, uint16_t size) {
    if (handle != ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_CLIENT_CONFIGURATION_HANDLE) return 0;
    uint8_t v[2] = {(uint8_t)bt_sc_ccc_get(con), (uint8_t)(bt_sc_ccc_get(con) >> 8)};
    return att_read_callback_handle_blob(v, sizeof v, off, buf, size);
}

/* Also called by BTstack with the stored value when an encrypted bonded peer reconnects (CCC restore). */
static int gatt_write(hci_con_handle_t con, uint16_t handle, uint16_t mode, uint16_t off, uint8_t *buf, uint16_t size) {
    (void)mode; (void)off;
    if (handle != ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_CLIENT_CONFIGURATION_HANDLE || size < 2) return 0;
    uint16_t v = (uint16_t)(buf[0] | (buf[1] << 8));
    bt_logf("CCC restored/written for handle 0x%04x: service changed = 0x%04x", (unsigned)con, (unsigned)v);
    if (bt_sc_ccc(con, v)) bt_sc_request(con);
    return 0;
}

static void gatt_service_init(void) {
    s_gatt_svc.start_handle = ATT_SERVICE_GATT_SERVICE_START_HANDLE;
    s_gatt_svc.end_handle = ATT_SERVICE_GATT_SERVICE_END_HANDLE;
    s_gatt_svc.read_callback = gatt_read;
    s_gatt_svc.write_callback = gatt_write;
    att_server_register_service_handler(&s_gatt_svc);
    att_server_register_packet_handler(att_pkt);
    static const bt_sc_store_t st = {sc_get, sc_put, sc_del};
    uint32_t cur = bt_sc_hash(profile_data, sizeof profile_data);
    bt_sc_init(&st, cur);
    bt_logf("ATT database hash %08x", (unsigned)cur);
}
#endif

/* ---- start-up ---- */

static void on_signal(int sig) {
    (void)sig;
    s_stop = 1;
    btstack_run_loop_trigger_exit();
}

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
struct hh_sockaddr_hci {
    unsigned short family, dev, channel;
};

/* Is there an HCI device N we could bind? (the transport itself waits a while for a late one) */
static int adapter_present(int hci) {
    int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
    if (s < 0) return 0;
    struct hh_sockaddr_hci a = {AF_BLUETOOTH, (unsigned short)hci, 0 /* RAW */};
    int ok = bind(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

static void mkdir_parents(const char *path) {
    char b[256];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(b, 0700);
        *p = '/';
    }
}

static int fail(uint8_t state, const char *why) {
    g_bt_state = state;
    log_msg(why);
    bt_push_state();
    ipc_flush_blocking();
    return 1;
}

int bt_helper_main(int argc, char **argv) {
    const char *keys = BT_DEFAULT_KEYS;
    long hci = 0, fd = BT_IPC_FD;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--hci") && i + 1 < argc) hci = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fd") && i + 1 < argc) fd = strtol(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "usage: hub bt-helper [--keys PATH] [--hci N] [--fd N]\n");
            return 2;
        }
    }
    if (hci < 0 || hci > 15 || fd < 0 || fd > 1023) {
        fprintf(stderr, "hub bt-helper: bad --hci/--fd\n");
        return 2;
    }

    /* a small, polite process: an address-space cap with margin over the hub image (BENCH: measure the
     * helper's real VmPeak and tighten), low priority. ASan needs the address space. */
#if !defined(__SANITIZE_ADDRESS__)
    struct rlimit rl = {32u << 20, 32u << 20};
    if (setrlimit(RLIMIT_AS, &rl) != 0) log_msg("setrlimit(RLIMIT_AS) failed");
#endif
    if (nice(5) == -1 && errno) log_msg("nice failed");

    g_bt_debug = getenv("HH_BT_DEBUG") != NULL;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    prctl(PR_SET_PDEATHSIG, SIGKILL); /* the parent is gone: so are we */
    if (fcntl((int)fd, F_GETFD) < 0) {
        fprintf(stderr, "hub bt-helper: no control channel on fd %ld\n", fd);
        return 2;
    }
    s_fd = (int)fd;
    fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL) | O_NONBLOCK);
    bt_push_state();

    if (!adapter_present((int)hci)) return fail(BT_ST_NO_ADAPTER, "no Bluetooth adapter");

    mkdir_parents(keys);
    {
        char dir[300], p[320];
        snprintf(dir, sizeof dir, "%s", keys);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        else snprintf(dir, sizeof dir, ".");
#ifdef HH_BT_LE
        snprintf(p, sizeof p, "%s/le_addr", dir);
        int ar = bt_leaddr_load_or_create(p, s_le_addr, NULL);
        s_le_addr_ok = ar >= 0;
        if (ar < 0) log_msg("LE address could not be stored: LE keeps the public address");
#endif
        snprintf(p, sizeof p, "%s/hci_trace", dir);
        if (access(p, F_OK) == 0) { /* bench aid: packets to a size-capped file in RAM */
            if (bt_trace_start(BT_TRACE_FILE, BT_TRACE_CAP) == 0) bt_logf("HCI trace on: %s (cap %u KiB)", BT_TRACE_FILE, BT_TRACE_CAP / 1024u);
            else log_msg("HCI trace file could not be opened");
        }
    }
    if (bt_bonds_prepare(keys) < 0) log_msg("bond file could not be rewritten");
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_transport_config_linux_t tcfg = {.type = HCI_TRANSPORT_CONFIG_LINUX, .device_id = (uint16_t)hci};
    hci_init(hci_transport_linux_instance(), &tcfg);

    g_tlv = btstack_tlv_posix_init_instance(&g_tlv_ctx, keys);
    btstack_tlv_set_instance(g_tlv, &g_tlv_ctx);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(g_tlv, &g_tlv_ctx));
#ifdef HH_BT_LE
    le_device_db_tlv_configure(g_tlv, &g_tlv_ctx);
#endif
    bt_hosts_load();
    bt_remotes_load();

    l2cap_init();
#ifdef HH_BT_LE
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION); /* Just Works; SC (software ECC), legacy when the peer has none */
    att_server_init(profile_data, NULL, NULL);
    gatt_service_init();
    battery_service_server_init(100);
    device_information_service_server_init();
    device_information_service_server_set_pnp_id(2 /* USB */, 0x046D, 0xC52B, 0x0100);
    hids_device_init_with_storage(0, bt_hid_descriptor, (uint16_t)bt_hid_descriptor_len, 3, s_hids_reports);
    hids_device_register_packet_handler(hids_handler);
    bt_remote_init();
    s_sm_cb.callback = &sm_handler;
    sm_add_event_handler(&s_sm_cb);
#endif

    static uint8_t hid_sdp[1024], did_sdp[128];
    sdp_init();
    hid_sdp_record_t hp = {
        .hid_device_subclass = 0x2540, .hid_country_code = 33, .hid_virtual_cable = 0, .hid_remote_wake = 1,
        .hid_reconnect_initiate = 1, .hid_normally_connectable = 1, .hid_boot_device = 1,
        .hid_ssr_host_max_latency = 1600, .hid_ssr_host_min_timeout = 3200, .hid_supervision_timeout = 3200,
        .hid_descriptor = bt_hid_descriptor, .hid_descriptor_size = (uint16_t)bt_hid_descriptor_len,
        .device_name = BT_NAME};
    hid_create_sdp_record(hid_sdp, sdp_create_service_record_handle(), &hp);
    sdp_register_service(hid_sdp);
    device_id_create_sdp_record(did_sdp, sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_USB,
                                0x046D, 0xC52B, 0x0100);
    sdp_register_service(did_sdp);
    hid_device_init(1, (uint16_t)bt_hid_descriptor_len, bt_hid_descriptor);
    hid_device_register_packet_handler(&hci_handler);
    bt_hid_init();

    s_hci_cb.callback = &hci_handler;
    hci_add_event_handler(&s_hci_cb);

    btstack_run_loop_set_data_source_fd(&s_ipc_ds, s_fd);
    btstack_run_loop_set_data_source_handler(&s_ipc_ds, &ipc_process);
    btstack_run_loop_add_data_source(&s_ipc_ds);
    btstack_run_loop_enable_data_source_callbacks(&s_ipc_ds, DATA_SOURCE_CALLBACK_READ);
    s_ipc_registered = true;
    tx_drain(); /* arms the writable callback when the early pushes are still queued */

    if (hci_power_control(HCI_POWER_ON) != 0) return fail(BT_ST_ERROR, "hci_power_control failed");
    if (!s_stop) btstack_run_loop_execute();
    hci_power_control(HCI_POWER_OFF);
    ipc_flush_blocking();
    return s_exit_code;
}
