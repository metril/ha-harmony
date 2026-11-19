/* `hub bt-helper [--keys PATH] [--hci N] [--fd N]`: Bluetooth HID helper. Classic HID device,
 * LE HID peripheral and LE HOGP host (HH_BT_LE) around BTstack, controlled by the parent over an
 * inherited socketpair fd (bt_ipc.h). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt_hid.h"
#include "bt_main.h"
#include "bt_pairing.h"
#include "bt_remote.h"
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

static int s_fd = -1;
static uint8_t s_rx[2 * (BT_IPC_MAX_PAYLOAD + BT_IPC_HDR)];
static size_t s_rx_n;
static btstack_data_source_t s_ipc_ds;
static volatile sig_atomic_t s_stop;

static void ipc_send(uint8_t type, const uint8_t *p, size_t n) {
    if (s_fd < 0) return;
    static uint8_t buf[BT_IPC_HDR + BT_IPC_MAX_PAYLOAD];
    size_t len = bt_ipc_frame(buf, type, p, n);
    if (!len) return;
    ssize_t w = write(s_fd, buf, len);
    if (w != (ssize_t)len && w >= 0) bt_logf("ipc: short write (%zd of %zu), frame lost", w, len);
    /* EAGAIN: the parent does not read; it asks again with GET */
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

void bt_push_hosts(void) {
    uint8_t *p = malloc(1 + (size_t)g_hosts_n * (10 + 1 + BT_NAME_MAX));
    if (!p) return;
    size_t o = 1;
    uint8_t n = 0;
    for (int i = 0; i < g_hosts_n; i++) {
        const bt_host_t *h = &g_hosts[i];
        if (!h->used) continue;
        p[o++] = (uint8_t)i;
        p[o++] = h->transport;
        p[o++] = h->addr_type;
        p[o++] = h->connected;
        memcpy(p + o, h->addr, 6);
        o += 6;
        o += put_name(p + o, h->name);
        n++;
    }
    p[0] = n;
    ipc_send(BT_IPC_HOSTS, p, o);
    free(p);
}

void bt_push_remotes(void) {
    uint8_t *p = malloc(1 + (size_t)g_remotes_n * (10 + 1 + BT_NAME_MAX));
    if (!p) return;
    size_t o = 1;
    uint8_t n = 0;
    for (int i = 0; i < g_remotes_n; i++) {
        const bt_remote_t *r = &g_remotes[i];
        if (!r->used) continue;
        p[o++] = (uint8_t)i;
        p[o++] = r->addr_type;
        p[o++] = r->connected;
        p[o++] = BT_BATTERY_UNKNOWN;
        memcpy(p + o, r->addr, 6);
        o += 6;
        o += put_name(p + o, r->name);
        n++;
    }
    p[0] = n;
    ipc_send(BT_IPC_REMOTES, p, o);
    free(p);
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

static void ipc_stop(void) {
    s_stop = 1;
    btstack_run_loop_trigger_exit();
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
        if (n >= 3) bt_pair_window(p[0] != 0, (uint16_t)((p[1] << 8) | p[2]), n >= 4 && p[3] == BT_KIND_REMOTE ? BT_KIND_REMOTE : BT_KIND_HOST);
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
    default:
        break;
    }
}

static void ipc_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
    (void)ds;
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
    if (g_bt_le) gap_set_connection_parameters(48, 48, 6, 12, 0, 200, 0, 0); /* fast central connects */
#endif
    g_bt_state = BT_ST_READY;
    push_all();
    bt_remote_scan_update();
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
        if (bt_pair_host_open()) gap_pin_code_response(a, "0000");
        else gap_pin_code_negative(a);
        break;
    }
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t a;
        hci_event_user_confirmation_request_get_bd_addr(packet, a);
        if (bt_pair_host_open()) gap_ssp_confirmation_response(a);
        else gap_ssp_confirmation_negative(a);
        break;
    }
    case HCI_EVENT_USER_PASSKEY_REQUEST: {
        bd_addr_t a;
        hci_event_user_passkey_request_get_bd_addr(packet, a);
        if (bt_pair_host_open()) gap_ssp_passkey_response(a, 0);
        else gap_ssp_passkey_negative(a);
        break;
    }
    case HCI_EVENT_LINK_KEY_NOTIFICATION: {
        bd_addr_t a;
        reverse_bd_addr(&packet[2], a);
        bt_classic_link_key(a);
        break;
    }
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        if (hci_event_simple_pairing_complete_get_status(packet) != ERROR_CODE_SUCCESS && bt_pair_host_open())
            bt_push_pairres(BT_KIND_HOST, BT_PR_FAILED, -1);
        break;
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
        bt_classic_acl_gone(h);
#ifdef HH_BT_LE
        bt_le_host_gone(h);
        bt_remote_gone(h);
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
    case SM_EVENT_JUST_WORKS_REQUEST: {
        hci_con_handle_t h = sm_event_just_works_request_get_handle(packet);
        if (bt_remote_owns_handle(h)) bt_remote_just_works(h);
        else if (bt_pair_host_open()) sm_just_works_confirm(h);
        else sm_bonding_decline(h); /* pairing only inside the window */
        break;
    }
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
        hci_con_handle_t h = sm_event_numeric_comparison_request_get_handle(packet);
        if (!bt_remote_owns_handle(h) && bt_pair_host_open()) sm_numeric_comparison_confirm(h);
        else sm_bonding_decline(h);
        break;
    }
    case SM_EVENT_PAIRING_COMPLETE: {
        hci_con_handle_t h = sm_event_pairing_complete_get_handle(packet);
        uint8_t st = sm_event_pairing_complete_get_status(packet);
        sm_event_pairing_complete_get_address(packet, a);
        uint8_t at = sm_event_pairing_complete_get_addr_type(packet);
        if (bt_remote_owns_handle(h)) bt_remote_pairing_complete(h, a, at, st);
        else if (st == ERROR_CODE_SUCCESS) bt_le_host_identified(h, a, at, true);
        else if (bt_pair_host_open()) bt_push_pairres(BT_KIND_HOST, BT_PR_FAILED, -1);
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
        if (s >= 0 && hids_subevent_input_report_enable_get_enable(packet)) bt_hid_ready(s);
        break;
    }
    default:
        break;
    }
}
static hids_device_report_t s_hids_reports[3];
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

    /* a small, polite process: 16 MB of address space, low priority (ASan needs the address space) */
#if !defined(__SANITIZE_ADDRESS__)
    struct rlimit rl = {16u << 20, 16u << 20};
    if (setrlimit(RLIMIT_AS, &rl) != 0) log_msg("setrlimit(RLIMIT_AS) failed");
#endif
    if (nice(5) == -1 && errno) log_msg("nice failed");

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (fcntl((int)fd, F_GETFD) >= 0) {
        s_fd = (int)fd;
        fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL) | O_NONBLOCK);
    } else {
        log_msg("no control channel on the inherited fd; running without IPC");
    }
    bt_push_state();

    if (!adapter_present((int)hci)) return fail(BT_ST_NO_ADAPTER, "no Bluetooth adapter");

    mkdir_parents(keys);
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
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING); /* legacy Just Works with bonding, no ECC */
    att_server_init(profile_data, NULL, NULL);
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

    if (s_fd >= 0) {
        btstack_run_loop_set_data_source_fd(&s_ipc_ds, s_fd);
        btstack_run_loop_set_data_source_handler(&s_ipc_ds, &ipc_process);
        btstack_run_loop_add_data_source(&s_ipc_ds);
        btstack_run_loop_enable_data_source_callbacks(&s_ipc_ds, DATA_SOURCE_CALLBACK_READ);
    }

    if (hci_power_control(HCI_POWER_ON) != 0) return fail(BT_ST_ERROR, "hci_power_control failed");
    if (!s_stop) btstack_run_loop_execute();
    hci_power_control(HCI_POWER_OFF);
    return 0;
}
