#ifndef HH_BT_PAIRING_H
#define HH_BT_PAIRING_H

#include "bt_state.h"

#define BT_PAIR_DEFAULT_S 120
#define BT_PAIR_MAX_S 600
#define BT_LE_RECONNECT_MS 15000 /* LE advertising for a host that was asked to connect */
#define BT_CLASSIC_ATTEMPTS 25   /* page attempts (1-2 s apart) before a Classic host is given up */

/* pairing window (mode: BT_KIND_HOST = discoverable/advertise, BT_KIND_REMOTE = scan and bond) */
void bt_pair_window(bool on, uint16_t secs, uint8_t mode, uint8_t transport); /* transport: BT_PAIR_TR_* (host mode) */
bool bt_pair_host_open(void);
bool bt_pair_classic_open(void); /* host window that accepts a new Classic bond */
bool bt_pair_le_open(void);      /* host window that accepts a new LE bond (and advertises for it) */
void bt_classic_scan_update(void);
extern uint8_t g_win_tr;
bool bt_pair_remote_open(void);
/* ends the open window with a result (also pushes PAIRRES); slot BT_SLOT_NONE when there is none */
void bt_pair_finish(uint8_t kind, uint8_t result, int slot);
extern bool g_classic_paging; /* a page attempt runs: LE scanning pauses */

/* host connections */
void bt_connect_slot(int slot);
void bt_forget_host(int slot);
void bt_pairing_controller_ready(void); /* HCI working: LE support known */

/* Classic events */
void bt_classic_link_key(const bd_addr_t addr);
void bt_classic_hid_opened(uint8_t status, uint16_t cid, const bd_addr_t addr, hci_con_handle_t handle);
void bt_classic_hid_closed(uint16_t cid);
void bt_classic_name(const bd_addr_t addr, const char *name);
void bt_classic_acl_gone(hci_con_handle_t handle);

/* LE peripheral (host) events */
void bt_le_host_connected(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type);
void bt_le_host_identified(hci_con_handle_t handle, const bd_addr_t addr, uint8_t addr_type, bool new_bond);
void bt_le_host_gone(hci_con_handle_t handle);
void bt_adv_update(void);

#endif
