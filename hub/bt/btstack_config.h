/* BTstack configuration for the hub's Bluetooth HID helper: Classic HID device (+ SDP, DeviceID)
* and, when HH_BT_LE is set, an LE peripheral (HID over GATT) and an LE central (HOGP host). MIPS32 big-endian, Linux raw HCI. */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

#define ENABLE_CLASSIC
#define ENABLE_SDP_SERVER
#define ENABLE_EXPLICIT_DEDICATED_BONDING_DISCONNECT

#ifdef HH_BT_LE
#define ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_CENTRAL     /* physical LE remotes (HOGP host) */
#define ENABLE_SOFTWARE_AES128 /* legacy LE pairing (Just Works) only: no ECC */
#endif

#define ENABLE_LOG_ERROR

#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define HCI_OUTGOING_PRE_BUFFER_SIZE 1

/* Bond storage. LE bonds (hosts and remotes together; every LE peer costs address resolution time)
 * and Classic link keys. The slot tables in bt_ipc.h are separate and grow at run time. */
#define HH_BT_LE_BONDS 64
#define NVM_NUM_DEVICE_DB_ENTRIES HH_BT_LE_BONDS
#define NVM_NUM_LINK_KEYS 255

#endif
