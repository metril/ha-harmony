#ifndef HH_BT_HID_H
#define HH_BT_HID_H

#include "bt_state.h"

#define BT_HID_SAFETY_MS 10000 /* a key held this long is released */
#define BT_HID_DEFER_MS 5000   /* a key for a host that is not connected waits this long */
#define BT_HID_SETTLE_MS 1500  /* a classic host that just woke gets its waiting keys after this */
#define BT_HID_CCC_MS 3000     /* an LE host that has not enabled the report CCCD by then loses the waiting keys */

extern const uint8_t bt_hid_descriptor[];
extern const size_t bt_hid_descriptor_len;

void bt_hid_init(void);
/* page 0x07 keyboard / 0x0C consumer; connects the host first when it is not connected */
void bt_hid_key(int slot, uint8_t page, uint16_t usage, bool down);
void bt_hid_connected(int slot, bool wait);
void bt_hid_ready(int slot); /* LE: the host enabled notifications */
void bt_hid_disconnected(int slot);
void bt_hid_drop_deferred(int slot);
bool bt_hid_slot_idle(int slot); /* nothing held, nothing waiting */
void bt_hid_can_send_now(uint16_t cid);

/* pure report logic (unit-testable): returns the report length (0 = none) in out[16] */
int bt_hid_apply(bt_hid_keys_t *k, uint8_t page, uint16_t usage, bool down, uint8_t out[16]);

#endif
