#include "bt_sc.h"

#include <string.h>

static struct {
    uint16_t handle, ccc;
    bool used, sent;
} s_sc[BT_SC_SLOTS];
static bool s_changed;

uint32_t bt_sc_hash(const uint8_t *db, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ db[i]) * 16777619u;
    return h;
}

bool bt_sc_db_changed(uint32_t cur, const uint8_t *stored, size_t len) {
    if (!stored || len != 4) return true;
    uint32_t old = (uint32_t)stored[0] | ((uint32_t)stored[1] << 8) | ((uint32_t)stored[2] << 16) | ((uint32_t)stored[3] << 24);
    return old != cur;
}

void bt_sc_value(uint8_t out[4]) {
    out[0] = 0x01; out[1] = 0x00; out[2] = 0xFF; out[3] = 0xFF;
}

void bt_sc_set_changed(bool changed) { s_changed = changed; }

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

bool bt_sc_ccc(uint16_t handle, uint16_t value) {
    int i = find(handle, true);
    if (i < 0) return false;
    s_sc[i].ccc = value;
    return s_changed && (value & 2) && !s_sc[i].sent;
}

void bt_sc_sent(uint16_t handle) {
    int i = find(handle, false);
    if (i >= 0) s_sc[i].sent = true;
}

void bt_sc_drop(uint16_t handle) {
    int i = find(handle, false);
    if (i >= 0) s_sc[i].used = false;
}

uint16_t bt_sc_ccc_get(uint16_t handle) {
    int i = find(handle, false);
    return i >= 0 ? s_sc[i].ccc : 0;
}
