#include "bt_state.h"

#include <stdarg.h>
#include <stdlib.h>

bt_host_t *g_hosts;
int g_hosts_n;
bt_remote_t *g_remotes;
int g_remotes_n;
btstack_tlv_posix_t g_tlv_ctx;
const btstack_tlv_t *g_tlv;
uint8_t g_bt_state = BT_ST_STARTING;
bool g_bt_le;
uint8_t g_window;

void log_msg(const char *msg) { fprintf(stderr, "bt-helper: %s\n", msg); }

bool g_bt_debug;
void bt_dbg(const char *msg) {
    if (g_bt_debug) fprintf(stderr, "bt-helper: [debug] %s\n", msg);
}

void bt_logf(const char *fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_msg(b);
}

/* BTstack's file store appends a record for every store_tag(), also for the value it already
 * holds, and never compacts the file: store a tag only when its value changes */
int bt_tlv_store_if_changed(uint32_t tag, const uint8_t *value, uint32_t len) {
    if (!g_tlv || !value || len == 0 || len > 64) return -1;
    uint8_t cur[64];
    int have = g_tlv->get_tag(&g_tlv_ctx, tag, cur, sizeof cur);
    if (have == (int)len && memcmp(cur, value, len) == 0) return 0;
    return g_tlv->store_tag(&g_tlv_ctx, tag, value, len);
}

/* ---- tables: realloc'd arrays indexed by slot, persisted as one TLV record per slot ---- */

/* record: transport u8, addr_type u8, addr[6], name[32] zero padded */
#define REC_LEN (2 + 6 + BT_NAME_MAX)
static uint32_t slot_tag(char kind, int slot) {
    return ((uint32_t)'H' << 24) | ((uint32_t)kind << 16) | (uint32_t)slot;
}

static bool grow_hosts(int n) {
    if (n > HH_BT_MAX_HOSTS) return false;
    if (n <= g_hosts_n) return true;
    bt_host_t *p = realloc(g_hosts, (size_t)n * sizeof *p);
    if (!p) return false;
    memset(p + g_hosts_n, 0, (size_t)(n - g_hosts_n) * sizeof *p);
    for (int i = g_hosts_n; i < n; i++) p[i].handle = HCI_CON_HANDLE_INVALID;
    g_hosts = p;
    g_hosts_n = n;
    return true;
}

static bool grow_remotes(int n) {
    if (n > HH_BT_MAX_REMOTES) return false;
    if (n <= g_remotes_n) return true;
    bt_remote_t *p = realloc(g_remotes, (size_t)n * sizeof *p);
    if (!p) return false;
    memset(p + g_remotes_n, 0, (size_t)(n - g_remotes_n) * sizeof *p);
    for (int i = g_remotes_n; i < n; i++) p[i].handle = HCI_CON_HANDLE_INVALID;
    g_remotes = p;
    g_remotes_n = n;
    return true;
}

static int le_bonds_used(void) {
    int n = 0;
    for (int i = 0; i < g_hosts_n; i++) n += g_hosts[i].used && g_hosts[i].transport == BT_TR_LE;
    for (int i = 0; i < g_remotes_n; i++) n += g_remotes[i].used;
    return n;
}

void bt_host_save(int slot) {
    if (slot < 0 || slot >= g_hosts_n || !g_tlv) return;
    bt_host_t *h = &g_hosts[slot];
    if (!h->used) {
        g_tlv->delete_tag(&g_tlv_ctx, slot_tag('S', slot));
        return;
    }
    uint8_t rec[REC_LEN] = {h->transport, h->addr_type};
    memcpy(rec + 2, h->addr, 6);
    memcpy(rec + 8, h->name, BT_NAME_MAX);
    bt_tlv_store_if_changed(slot_tag('S', slot), rec, REC_LEN);
}

void bt_remote_save(int slot) {
    if (slot < 0 || slot >= g_remotes_n || !g_tlv) return;
    bt_remote_t *r = &g_remotes[slot];
    if (!r->used) {
        g_tlv->delete_tag(&g_tlv_ctx, slot_tag('R', slot));
        return;
    }
    uint8_t rec[REC_LEN] = {BT_TR_LE, r->addr_type};
    memcpy(rec + 2, r->addr, 6);
    memcpy(rec + 8, r->name, BT_NAME_MAX);
    bt_tlv_store_if_changed(slot_tag('R', slot), rec, REC_LEN);
}

void bt_hosts_load(void) {
    for (int i = 0; i < HH_BT_MAX_HOSTS && g_tlv; i++) {
        uint8_t rec[REC_LEN];
        if (g_tlv->get_tag(&g_tlv_ctx, slot_tag('S', i), rec, sizeof rec) != REC_LEN || !grow_hosts(i + 1)) continue;
        bt_host_t *h = &g_hosts[i];
        h->used = true;
        h->transport = rec[0] == BT_TR_LE ? BT_TR_LE : BT_TR_CLASSIC;
        h->addr_type = rec[1];
        memcpy(h->addr, rec + 2, 6);
        memcpy(h->name, rec + 8, BT_NAME_MAX);
        h->name[BT_NAME_MAX] = 0;
    }
}

void bt_remotes_load(void) {
    for (int i = 0; i < HH_BT_MAX_REMOTES && g_tlv; i++) {
        uint8_t rec[REC_LEN];
        if (g_tlv->get_tag(&g_tlv_ctx, slot_tag('R', i), rec, sizeof rec) != REC_LEN || !grow_remotes(i + 1)) continue;
        bt_remote_t *r = &g_remotes[i];
        r->used = true;
        r->addr_type = rec[1];
        memcpy(r->addr, rec + 2, 6);
        memcpy(r->name, rec + 8, BT_NAME_MAX);
        r->name[BT_NAME_MAX] = 0;
    }
}

int bt_host_find_addr(const bd_addr_t addr, uint8_t transport) {
    for (int i = 0; i < g_hosts_n; i++)
        if (g_hosts[i].used && g_hosts[i].transport == transport && memcmp(g_hosts[i].addr, addr, 6) == 0) return i;
    return -1;
}

int bt_host_find_cid(uint16_t cid) {
    for (int i = 0; cid && i < g_hosts_n; i++)
        if (g_hosts[i].used && g_hosts[i].connected && g_hosts[i].cid == cid) return i;
    return -1;
}

int bt_host_find_handle(hci_con_handle_t hd) {
    for (int i = 0; hd != HCI_CON_HANDLE_INVALID && i < g_hosts_n; i++)
        if (g_hosts[i].used && g_hosts[i].connected && g_hosts[i].handle == hd) return i;
    return -1;
}

int bt_host_alloc(uint8_t transport, uint8_t addr_type, const bd_addr_t addr, const char *name) {
    if (transport == BT_TR_LE && le_bonds_used() >= HH_BT_LE_BONDS) return -1;
    int slot = -1;
    for (int i = 0; i < g_hosts_n && slot < 0; i++)
        if (!g_hosts[i].used) slot = i;
    if (slot < 0) {
        slot = g_hosts_n;
        if (!grow_hosts(g_hosts_n + 1)) return -1;
    }
    bt_host_t *h = &g_hosts[slot];
    memset(h, 0, sizeof *h);
    h->used = true;
    h->transport = transport;
    h->addr_type = addr_type;
    memcpy(h->addr, addr, 6);
    h->handle = HCI_CON_HANDLE_INVALID;
    snprintf(h->name, sizeof h->name, "%s", name ? name : "");
    return slot;
}

void bt_host_free(int slot) {
    memset(&g_hosts[slot], 0, sizeof g_hosts[slot]);
    g_hosts[slot].handle = HCI_CON_HANDLE_INVALID;
    bt_host_save(slot);
}

int bt_hosts_connected(void) {
    int n = 0;
    for (int i = 0; i < g_hosts_n; i++) n += g_hosts[i].used && g_hosts[i].connected;
    return n;
}

int bt_remote_alloc(uint8_t addr_type, const bd_addr_t addr, const char *name) {
    if (le_bonds_used() >= HH_BT_LE_BONDS) return -1;
    int slot = -1;
    for (int i = 0; i < g_remotes_n && slot < 0; i++)
        if (!g_remotes[i].used) slot = i;
    if (slot < 0) {
        slot = g_remotes_n;
        if (!grow_remotes(g_remotes_n + 1)) return -1;
    }
    bt_remote_t *r = &g_remotes[slot];
    memset(r, 0, sizeof *r);
    r->used = true;
    r->addr_type = addr_type;
    memcpy(r->addr, addr, 6);
    r->handle = HCI_CON_HANDLE_INVALID;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    return slot;
}

void bt_remote_free(int slot) {
    memset(&g_remotes[slot], 0, sizeof g_remotes[slot]);
    g_remotes[slot].handle = HCI_CON_HANDLE_INVALID;
    bt_remote_save(slot);
}

int bt_remote_find_addr(const bd_addr_t addr) {
    for (int i = 0; i < g_remotes_n; i++)
        if (g_remotes[i].used && memcmp(g_remotes[i].addr, addr, 6) == 0) return i;
    return -1;
}
