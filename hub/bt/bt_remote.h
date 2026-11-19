#ifndef HH_BT_REMOTE_H
#define HH_BT_REMOTE_H

/* Physical LE HID remotes (the hub is the LE central / HOGP host): scan, bond inside the pairing
 * window, reconnect when a bonded remote advertises, decode its reports to key events. */
#include "bt_state.h"

void bt_remote_init(void);
void bt_remote_scan_update(void);        /* start or stop scanning as the state wants it */
void bt_remote_forget(int slot);

/* BTstack events routed here by bt_main.c */
void bt_remote_adv_report(const uint8_t *packet);
bool bt_remote_owns_handle(hci_con_handle_t handle);
/* an LE link we initiated is up; false when no attempt was pending (the caller disconnects) */
bool bt_remote_le_connected(hci_con_handle_t handle);
void bt_remote_le_connect_failed(void);
void bt_remote_just_works(hci_con_handle_t handle);
void bt_remote_pairing_complete(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type, uint8_t status);
void bt_remote_reencrypted(hci_con_handle_t handle, uint8_t status);
void bt_remote_gone(hci_con_handle_t handle);

/* HID report -> key transitions (page, usage); exposed for tests. Returns the number of events. */
/* turns the new set of active usages into press/release events against the old one, updates old */
typedef void (*bt_rkey_emit_fn)(uint8_t page, uint16_t usage, bool down, void *ctx);
void bt_rkey_diff(bt_rkey_set_t *old, const bt_rkey_set_t *now, bt_rkey_emit_fn emit, void *ctx);

#endif
