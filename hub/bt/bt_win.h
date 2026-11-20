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

/* One log line for a pairing failure: transport, peer, the stack's reason. No key material. */
static inline void bt_pair_fail_line(char *out, size_t n, const char *transport, const char *peer, const char *what,
                                     unsigned code) {
    snprintf(out, n, "pairing failed: transport %s, peer %s, %s 0x%02x", transport, peer, what, code);
}

#endif
