#include "bt_hid.h"

#include "bt_hid_queue.h"
#include "bt_pairing.h"
#ifdef HH_BT_LE
#include "ble/gatt-service/hids_device.h"
#endif

/* Report map shared by Classic (SDP) and LE (HIDS): keyboard (report id 1), consumer (id 2) */
const uint8_t bt_hid_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,       /* Generic Desktop, Keyboard, Application */
    0x85, 0x01,                               /*   Report ID 1 */
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,       /*   Key codes E0..E7: modifiers */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,       /*   reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs out */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,       /*   LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, /* 6 keys */
    0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,       /* Consumer Control */
    0x85, 0x02,                               /*   Report ID 2 */
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xc0,
};
const size_t bt_hid_descriptor_len = sizeof bt_hid_descriptor;

/* ---- report logic ---- */

/* Reports as sent on the Classic interrupt channel: 0xA1, report id, data. The LE path strips
 * the first two bytes (the id is the characteristic). */
int bt_hid_apply(bt_hid_keys_t *k, uint8_t page, uint16_t usage, bool down, uint8_t out[16]) {
    if (page == 0x07) {
        if (usage >= 0xE0 && usage <= 0xE7) {
            uint8_t bit = (uint8_t)(1u << (usage - 0xE0));
            k->mod = down ? (uint8_t)(k->mod | bit) : (uint8_t)(k->mod & ~bit);
        } else if (usage > 0 && usage < 0xE0) {
            int at = -1, free_at = -1;
            for (int i = 0; i < 6; i++) {
                if (k->keys[i] == usage) at = i;
                if (!k->keys[i] && free_at < 0) free_at = i;
            }
            if (down && at < 0 && free_at >= 0) k->keys[free_at] = (uint8_t)usage;
            if (!down && at >= 0) k->keys[at] = 0;
        } else {
            return 0;
        }
        out[0] = 0xA1; out[1] = 0x01; out[2] = k->mod; out[3] = 0;
        memcpy(out + 4, k->keys, 6);
        return 10;
    }
    if (page == 0x0C && usage < 0x400) {
        k->cons = down ? usage : 0;
        out[0] = 0xA1; out[1] = 0x02; out[2] = (uint8_t)(k->cons & 0xFF); out[3] = (uint8_t)(k->cons >> 8);
        return 4;
    }
    return 0;
}

/* ---- queue on BTstack ---- */

#define HIDQ_RETRY_MS 20
static btstack_timer_source_t s_hidq_timer;
static bool s_hidq_ready;

static uint8_t hidq_btstack_send(uint16_t cid, const uint8_t *report, uint16_t len) {
#ifdef HH_BT_LE
    if (cid & BT_LE_CID_FLAG)
        return hids_device_send_input_report_for_id((hci_con_handle_t)(cid & ~BT_LE_CID_FLAG), report[1],
                                                    report + 2, (uint16_t)(len - 2));
#endif
    return hid_device_send_interrupt_message(cid, report, len);
}

static void hidq_btstack_request(uint16_t cid) {
#ifdef HH_BT_LE
    if (cid & BT_LE_CID_FLAG) {
        hids_device_request_can_send_now_event((hci_con_handle_t)(cid & ~BT_LE_CID_FLAG));
        return;
    }
#endif
    hid_device_request_can_send_now_event(cid);
}

static void hidq_timer_handler(btstack_timer_source_t *ts);
static void hidq_arm_timer(void) {
    btstack_run_loop_remove_timer(&s_hidq_timer);
    if (!hidq_pending_any()) return;
    btstack_run_loop_set_timer_handler(&s_hidq_timer, hidq_timer_handler);
    btstack_run_loop_set_timer(&s_hidq_timer, HIDQ_RETRY_MS);
    btstack_run_loop_add_timer(&s_hidq_timer);
}
static void hidq_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    hidq_service_all(btstack_run_loop_get_time_ms());
    hidq_arm_timer();
}

static void send_report(uint16_t cid, const uint8_t *r, uint16_t len) {
    hidq_send(cid, r, len, btstack_run_loop_get_time_ms());
    hidq_arm_timer();
}

void bt_hid_can_send_now(uint16_t cid) {
    hidq_service(cid, btstack_run_loop_get_time_ms());
    hidq_arm_timer();
}

/* ---- per-slot key state, waiting keys, safety release ---- */

typedef struct {
    uint8_t slot;
    uint8_t page, down;
    uint16_t usage;
    uint32_t t_ms;
} deferred_t;
#define DEFER_MAX 16
static deferred_t s_def[DEFER_MAX];
static int s_def_n;
static btstack_timer_source_t s_tick, s_settle;

static bool held(const bt_hid_keys_t *k) {
    if (k->mod || k->cons) return true;
    for (int i = 0; i < 6; i++) if (k->keys[i]) return true;
    return false;
}

static void tick_arm(void) {
    bool need = s_def_n > 0;
    for (int i = 0; i < g_hosts_n && !need; i++) need = g_hosts[i].deadline != 0;
    btstack_run_loop_remove_timer(&s_tick);
    if (!need) return;
    btstack_run_loop_set_timer(&s_tick, 250);
    btstack_run_loop_add_timer(&s_tick);
}

static void release_all(int slot) {
    bt_host_t *h = &g_hosts[slot];
    uint8_t r[16];
    bt_hid_keys_t *k = &g_hosts[slot].keys;
    bool kb = k->mod || k->keys[0] || k->keys[1] || k->keys[2] || k->keys[3] || k->keys[4] || k->keys[5];
    bool cs = k->cons != 0;
    memset(k, 0, sizeof *k);
    g_hosts[slot].deadline = 0;
    if (!h->used || !h->connected) return;
    if (kb) { int n = bt_hid_apply(k, 0x07, 0, false, r); (void)n; memset(r, 0, sizeof r); r[0] = 0xA1; r[1] = 1; send_report(h->cid, r, 10); }
    if (cs) { memset(r, 0, sizeof r); r[0] = 0xA1; r[1] = 2; send_report(h->cid, r, 4); }
}

static void apply_send(int slot, uint8_t page, uint16_t usage, bool down) {
    bt_host_t *h = &g_hosts[slot];
    uint8_t r[16];
    int n = bt_hid_apply(&g_hosts[slot].keys, page, usage, down, r);
    if (n <= 0 || !h->connected) return;
    g_hosts[slot].deadline = held(&g_hosts[slot].keys) ? btstack_run_loop_get_time_ms() + BT_HID_SAFETY_MS : 0;
    if (down && !g_hosts[slot].deadline) g_hosts[slot].deadline = 0;
    send_report(h->cid, r, (uint16_t)n);
}

static void tick(btstack_timer_source_t *ts) {
    (void)ts;
    uint32_t now = btstack_run_loop_get_time_ms();
    for (int i = 0; i < s_def_n;) {
        if (!g_hosts[s_def[i].slot].connected && now - s_def[i].t_ms > BT_HID_DEFER_MS) {
            memmove(&s_def[i], &s_def[i + 1], (size_t)(s_def_n - i - 1) * sizeof s_def[0]);
            s_def_n--;
        } else {
            i++;
        }
    }
    for (int i = 0; i < g_hosts_n; i++) {
        if (g_hosts[i].deadline && (int32_t)(now - g_hosts[i].deadline) >= 0) {
            bt_logf("slot %d: key held over %d s, released", i, BT_HID_SAFETY_MS / 1000);
            release_all(i);
        }
    }
    tick_arm();
}

static bool has_deferred(int slot) {
    for (int i = 0; i < s_def_n; i++) if (s_def[i].slot == slot) return true;
    return false;
}

void bt_hid_init(void) {
    if (s_hidq_ready) return;
    hidq_init(hidq_btstack_send, hidq_btstack_request, log_msg);
    hidq_set_debug_log(bt_dbg); /* the stack refusing a report and the later send: routine back-pressure */
    btstack_run_loop_set_timer_handler(&s_tick, tick);
    s_hidq_ready = true;
}

void bt_hid_key(int slot, uint8_t page, uint16_t usage, bool down) {
    if (slot < 0 || slot >= g_hosts_n || !g_hosts[slot].used) return;
    bt_host_t *h = &g_hosts[slot];
    h->last_used_ms = btstack_run_loop_get_time_ms();
    if (h->connected && !h->settling && !has_deferred(slot)) {
        apply_send(slot, page, usage, down);
    } else {
        if (s_def_n == DEFER_MAX) {
            memmove(&s_def[0], &s_def[1], (DEFER_MAX - 1) * sizeof s_def[0]);
            s_def_n--;
        }
        s_def[s_def_n++] = (deferred_t){(uint8_t)slot, page, down, usage, h->last_used_ms};
        if (!h->connected) bt_connect_slot(slot);
    }
    tick_arm();
}

static void flush_slot(int s) {
    g_hosts[s].settling = false;
    if (!g_hosts[s].connected) return;
    for (int i = 0; i < s_def_n;) {
        if (s_def[i].slot != s) { i++; continue; }
        deferred_t d = s_def[i];
        memmove(&s_def[i], &s_def[i + 1], (size_t)(s_def_n - i - 1) * sizeof s_def[0]);
        s_def_n--;
        apply_send(s, d.page, d.usage, d.down);
    }
}

static void settle_done(btstack_timer_source_t *ts) {
    (void)ts;
    for (int s = 0; s < g_hosts_n; s++)
        if (g_hosts[s].used && g_hosts[s].settling) flush_slot(s);
    tick_arm();
}

/* wait: keep keys back until bt_hid_ready() or BT_HID_SETTLE_MS (LE: until the host subscribes) */
void bt_hid_connected(int slot, bool wait) {
    memset(&g_hosts[slot].keys, 0, sizeof g_hosts[slot].keys);
    g_hosts[slot].deadline = 0;
    if (!wait && !has_deferred(slot)) return;
    g_hosts[slot].settling = true;
    btstack_run_loop_set_timer_handler(&s_settle, settle_done);
    btstack_run_loop_remove_timer(&s_settle);
    btstack_run_loop_set_timer(&s_settle, BT_HID_SETTLE_MS);
    btstack_run_loop_add_timer(&s_settle);
}

void bt_hid_ready(int slot) {
    if (g_hosts[slot].settling) flush_slot(slot);
    tick_arm();
}

void bt_hid_drop_deferred(int slot) {
    for (int i = 0; i < s_def_n;) {
        if (s_def[i].slot == slot) {
            memmove(&s_def[i], &s_def[i + 1], (size_t)(s_def_n - i - 1) * sizeof s_def[0]);
            s_def_n--;
        } else {
            i++;
        }
    }
}

/* the link is gone (the cid is still in the slot): the host releases all keys itself */
void bt_hid_disconnected(int slot) {
    hidq_drop(g_hosts[slot].cid);
    hidq_arm_timer();
    memset(&g_hosts[slot].keys, 0, sizeof g_hosts[slot].keys);
    g_hosts[slot].deadline = 0;
    g_hosts[slot].settling = false;
}

bool bt_hid_slot_idle(int slot) {
    return !g_hosts[slot].deadline && !has_deferred(slot) && !(g_hosts[slot].cid && hidq_pending(g_hosts[slot].cid));
}
