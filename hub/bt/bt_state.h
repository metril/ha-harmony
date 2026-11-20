#ifndef HH_BT_STATE_H
#define HH_BT_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "bt_ipc.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "btstack_tlv_posix.h"
#include "hci.h"
#include "gap.h"
#include "l2cap.h"
#include "btstack_util.h"
#include "classic/hid_device.h"
#ifdef HH_BT_LE
#include "ble/sm.h"
#include "ble/att_server.h"
#endif

#define BT_DEFAULT_KEYS "/data/ha-harmony/bt/keys.tlv"
#define BT_LE_CID_FLAG 0x8000 /* hid queue id of an LE link: flag | connection handle */

typedef struct {
    uint8_t mod, keys[6];
    uint16_t cons;
} bt_hid_keys_t;

typedef struct {
    uint8_t page;
    uint16_t usage;
} bt_rkey_t;
#define BT_RKEY_MAX 16
typedef struct {
    bt_rkey_t act[BT_RKEY_MAX];
    int n;
} bt_rkey_set_t;

/* One bonded host (a slot). Persisted in the TLV store next to the link keys / LE bonds. */
typedef struct {
    bool used;
    uint8_t transport; /* BT_TR_* */
    uint8_t addr_type; /* LE: bd_addr_type_t of the identity address; Classic: 0 */
    bd_addr_t addr;
    char name[BT_NAME_MAX + 1];
    /* runtime */
    bool connected;
    bool settling;     /* just connected: keys wait a moment (see bt_hid.c) */
    uint32_t settle_dl; /* when settling ends (run-loop ms), per slot */
    uint16_t cid;      /* Classic hid_cid, or BT_LE_CID_FLAG | LE handle */
    hci_con_handle_t handle;
    uint32_t last_used_ms;
    bt_hid_keys_t keys; /* keys held now */
    uint32_t deadline;  /* when a held key is released; 0 = none held */
} bt_host_t;

/* One bonded LE remote (central role, HOGP host). */
typedef struct {
    bool used;
    uint8_t addr_type;
    bd_addr_t addr;
    char name[BT_NAME_MAX + 1];
    bool connected;
    hci_con_handle_t handle;
    uint16_t hids_cid;
    bool pending;         /* new bond waiting for its HID service: not saved, not reported */
    uint8_t fails;        /* consecutive failed connects (backoff) */
    uint32_t next_try_ms; /* no new attempt before this */
    bt_rkey_set_t active; /* usages down now */
} bt_remote_t;

extern bt_host_t *g_hosts; /* grown with realloc, up to HH_BT_MAX_HOSTS; slot = index */
extern int g_hosts_n;
extern bt_remote_t *g_remotes;
extern int g_remotes_n;
extern btstack_tlv_posix_t g_tlv_ctx;
extern const btstack_tlv_t *g_tlv;
extern uint8_t g_bt_state; /* BT_ST_* */
extern bool g_bt_le;       /* LE usable (built in and the controller supports it) */
extern uint8_t g_window; /* 0 closed, 1 host window, 2 remote window */

void log_msg(const char *msg);
extern bool g_bt_debug;      /* HH_BT_DEBUG set: bt_dbg() lines are logged */
void bt_dbg(const char *msg); /* debug level (off by default) */
void bt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int bt_tlv_store_if_changed(uint32_t tag, const uint8_t *value, uint32_t len);

int bt_host_find_addr(const bd_addr_t addr, uint8_t transport);
int bt_host_find_cid(uint16_t cid);
int bt_host_find_handle(hci_con_handle_t h);
int bt_host_alloc(uint8_t transport, uint8_t addr_type, const bd_addr_t addr, const char *name); /* -1 full */
void bt_host_free(int slot);
void bt_host_save(int slot);
void bt_hosts_load(void);
int bt_hosts_connected(void);
int bt_remote_alloc(uint8_t addr_type, const bd_addr_t addr, const char *name); /* -1 full */
void bt_remote_free(int slot);
void bt_remote_save(int slot);
void bt_remotes_load(void);
int bt_remote_find_addr(const bd_addr_t addr);

/* IPC pushes, implemented by bt_main.c */
void bt_push_state(void);
void bt_push_hosts(void);
void bt_push_remotes(void);
void bt_push_pairres(uint8_t kind, uint8_t result, int slot);
void bt_push_candidate(const bd_addr_t addr, uint8_t addr_type, int8_t rssi, const char *name);
void bt_transport_fatal(int err); /* implemented by bt_main.c */
void bt_push_rkey(int slot, uint8_t page, uint16_t usage, bool down);

#endif
