#include "bt_sc.h"

#include <string.h>

static struct {
    uint16_t handle, ccc;
    uint8_t addr[6], type;
    bool used, known, sent;
} s_sc[BT_SC_SLOTS];
static bt_sc_store_t s_store;
static uint32_t s_cur;

uint32_t bt_sc_hash(const uint8_t *db, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ db[i]) * 16777619u;
    return h;
}

void bt_sc_init(const bt_sc_store_t *store, uint32_t cur_hash) {
    memset(s_sc, 0, sizeof s_sc);
    memset(&s_store, 0, sizeof s_store);
    if (store) s_store = *store;
    s_cur = cur_hash;
}

void bt_sc_value(uint8_t out[4]) {
    out[0] = 0x01; out[1] = 0x00; out[2] = 0xFF; out[3] = 0xFF;
}

/* "S" + 24 bits of FNV-1a over the identity address: one 4-byte record per bond */
static uint32_t tag_for(const uint8_t addr[6], uint8_t type) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ addr[i]) * 16777619u;
    h = (h ^ type) * 16777619u;
    return 0x53000000u | (h & 0x00FFFFFFu);
}

static void store_cur(const uint8_t addr[6], uint8_t type) {
    uint8_t v[4] = {(uint8_t)s_cur, (uint8_t)(s_cur >> 8), (uint8_t)(s_cur >> 16), (uint8_t)(s_cur >> 24)};
    if (s_store.put) s_store.put(tag_for(addr, type), v, sizeof v);
}

/* the stored hash differs from the running database, or none is stored (a bond from before this record) */
static bool stale(const uint8_t addr[6], uint8_t type) {
    uint8_t v[4];
    if (!s_store.get || s_store.get(tag_for(addr, type), v, sizeof v) != 4) return true;
    return ((uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24)) != s_cur;
}

void bt_sc_bond_fresh(const uint8_t addr[6], uint8_t addr_type) { store_cur(addr, addr_type); }

static int find(uint16_t handle, bool create) {
    int free_i = -1;
    for (int i = 0; i < BT_SC_SLOTS; i++) {
        if (s_sc[i].used && s_sc[i].handle == handle) return i;
        if (!s_sc[i].used && free_i < 0) free_i = i;
    }
    if (create && free_i >= 0) {
        memset(&s_sc[free_i], 0, sizeof s_sc[free_i]);
        s_sc[free_i].used = true;
        s_sc[free_i].handle = handle;
    }
    return create ? free_i : -1;
}

static bool pending(int i) {
    return i >= 0 && s_sc[i].known && (s_sc[i].ccc & 2) && !s_sc[i].sent && stale(s_sc[i].addr, s_sc[i].type);
}

bool bt_sc_peer(uint16_t handle, const uint8_t addr[6], uint8_t addr_type) {
    int i = find(handle, true);
    if (i < 0) return false;
    memcpy(s_sc[i].addr, addr, 6);
    s_sc[i].type = addr_type;
    s_sc[i].known = true;
    return pending(i);
}

bool bt_sc_ccc(uint16_t handle, uint16_t value) {
    int i = find(handle, true);
    if (i < 0) return false;
    s_sc[i].ccc = value;
    return pending(i);
}

void bt_sc_sent(uint16_t handle) {
    int i = find(handle, false);
    if (i >= 0) s_sc[i].sent = true;
}

void bt_sc_confirmed(uint16_t handle) {
    int i = find(handle, false);
    if (i >= 0 && s_sc[i].known) store_cur(s_sc[i].addr, s_sc[i].type);
}

void bt_sc_drop(uint16_t handle) {
    int i = find(handle, false);
    if (i >= 0) s_sc[i].used = false;
}

uint16_t bt_sc_ccc_get(uint16_t handle) {
    int i = find(handle, false);
    return i >= 0 ? s_sc[i].ccc : 0;
}
