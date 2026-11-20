#ifndef HH_BT_WIN_H
#define HH_BT_WIN_H
/* What a host pairing window allows, as pure functions (no stack calls) so they are unit tested.
 * window: 0 closed, 1 host, 2 remote (g_window). tr: BT_PAIR_TR_* the window was opened with. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bt_ipc.h"

/* Classic: inquiry scan (discoverable) and new Classic bonds only in a host window that is not LE-only */
static inline bool bt_win_classic_pairing(uint8_t window, uint8_t tr) { return window == 1 && tr != BT_PAIR_TR_LE; }
/* LE: connectable advertising as "general discoverable" and new LE bonds only in a host window that is not Classic-only */
static inline bool bt_win_le_pairing(uint8_t window, uint8_t tr) { return window == 1 && tr != BT_PAIR_TR_CLASSIC; }
/* Classic page scan: always on outside a window; in an LE-only window only while a Classic bond exists (so that
 * bonded Classic hosts can still reconnect; unbonded ones cannot pair: PIN/SSP are refused). */
static inline bool bt_win_classic_connectable(uint8_t window, uint8_t tr, bool classic_bond) {
    return !(window == 1 && tr == BT_PAIR_TR_LE) || classic_bond;
}

/* LE advertising data: the Flags byte says "general discoverable" (0x06) only while a window takes new LE bonds */
static inline uint8_t bt_win_adv_flags(bool pairing) { return pairing ? 0x06 : 0x04; }
enum { BT_ADV_NONE, BT_ADV_START, BT_ADV_STOP, BT_ADV_REFRESH };
/* What to do with advertising: `on` now, `want` wanted, flags in the data now / wanted. REFRESH = on already, but the
 * window opened or closed since the data was set: replace the data without stopping. */
static inline int bt_win_adv_action(bool on, bool want, uint8_t cur_flags, uint8_t new_flags) {
    if (want != on) return want ? BT_ADV_START : BT_ADV_STOP;
    return want && cur_flags != new_flags ? BT_ADV_REFRESH : BT_ADV_NONE;
}

/* One log line for a pairing failure: transport, peer, the stack's reason. No key material. */
static inline void bt_pair_fail_line(char *out, size_t n, const char *transport, const char *peer, const char *what,
                                     unsigned code) {
    snprintf(out, n, "pairing failed: transport %s, peer %s, %s 0x%02x", transport, peer, what, code);
}

#endif
