/* Wire format between the hub daemon (parent) and `hub bt-helper` (child), shared by both sides.
 *
 * Transport: a SOCK_STREAM socketpair; the child's end is inherited as fd 3 (--fd N overrides).
 * Frame:   type (u8) | len (u16, big endian) | payload (len bytes, len <= BT_IPC_MAX_PAYLOAD).
 * All integers are big endian. A peer that reads EOF treats the other side as gone: the helper
 * exits when the parent closes its end. Unknown frame types are ignored.
 * A 6-byte address is stored in display order (aa:bb:cc:dd:ee:ff -> aa bb cc dd ee ff).
 *
 * Two kinds of bonded peers, with separate slot tables:
 *   host   (kind 0)  a device the hub types INTO: Classic HID or LE HID peripheral. Slots
 *                    0..HH_BT_MAX_HOSTS-1 (254). A key for a host that is not connected connects it first.
 *   remote (kind 1)  a physical LE HID remote typed ON: the hub is the LE central (HOGP host).
 *                    Slots 0..HH_BT_MAX_REMOTES-1; reconnected by the helper when they advertise.
 *
 * Parent -> helper
 *   PING    0x01  token u8                      answered with PONG
 *   KEY     0x02  slot u8, page u8, usage u16, down u8 (1 = press, 0 = release)   [to a host]
 *                 page 0x07 keyboard/keypad, page 0x0C consumer. Queued up to 5 s while the host
 *                 is connecting; a key held 10 s is released.
 *   PAIR    0x03  on u8, secs u16, mode u8      pairing window (secs 0 = default 120, max 600).
 *                 mode 0 = host: discoverable (Classic) + advertising (LE); 1 = remote: scan for an
 *                 advertising HID remote and bond with it. Pairing is accepted only while open;
 *                 one mode at a time (a new window replaces the open one).
 *   FORGET  0x04  kind u8, slot u8              drop the bond and the slot
 *   CONNECT 0x05  slot u8                       connect a host now (Classic: page; LE: advertise)
 *   GET     0x06  (empty)                       push STATE, HOSTS and REMOTES again
 * Helper -> parent
 *   PONG    0x81  token u8
 *   STATE   0x82  state u8, window u8, le u8, hosts_max u8, conn_max u8, remotes_max u8
 *                 window: 0 closed, 1 host window open, 2 remote window open.
 *                 le: 0 when the build or the controller has no LE (Classic only, runtime).
 *                 hosts_max is the slot capacity (255), conn_max the simultaneous host connections.
 *   PAIRRES 0x83  kind u8, result u8, slot u8 (0xFF = none), addr[6], transport u8, name_len u8, name
 *   HOSTS   0x84  n u8, n x { slot u8, transport u8, addr_type u8, connected u8, addr[6], name_len u8, name }
 *   REMOTES 0x85  n u8, n x { slot u8, addr_type u8, connected u8, battery u8 (0xFF unknown), addr[6],
 *                              name_len u8, name }
 *   RKEY    0x86  remote slot u8, page u8, usage u16, down u8       key from a remote (page 0x07/0x0C)
 * STATE, HOSTS and REMOTES are pushed at start and on every change.
 */
#ifndef HH_BT_IPC_H
#define HH_BT_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HH_BT_MAX_HOSTS 255  /* bonded hosts = slots 0..254 (0xFF = none); the table grows on demand */
#define HH_BT_MAX_CONN 4     /* simultaneous host connections, Classic and LE together */
#define HH_BT_MAX_REMOTES 32 /* bonded LE remotes, slots 0..31 */

#define BT_IPC_FD 3
#define BT_IPC_MAX_PAYLOAD 16384 /* HOSTS with 255 entries is about 11 KB */
#define BT_IPC_HDR 3
#define BT_NAME_MAX 32

enum { BT_IPC_PING = 0x01, BT_IPC_KEY = 0x02, BT_IPC_PAIR = 0x03, BT_IPC_FORGET = 0x04,
       BT_IPC_CONNECT = 0x05, BT_IPC_GET = 0x06 };
enum { BT_IPC_PONG = 0x81, BT_IPC_STATE = 0x82, BT_IPC_PAIRRES = 0x83, BT_IPC_HOSTS = 0x84,
       BT_IPC_REMOTES = 0x85, BT_IPC_RKEY = 0x86 };

/* STATE.state */
enum { BT_ST_STARTING = 0, BT_ST_READY = 1, BT_ST_NO_ADAPTER = 2, BT_ST_ERROR = 3 };
/* kind of a bonded peer */
enum { BT_KIND_HOST = 0, BT_KIND_REMOTE = 1 };
/* host transport */
enum { BT_TR_CLASSIC = 0, BT_TR_LE = 1 };
/* PAIRRES.result */
enum { BT_PR_OK = 0, BT_PR_TIMEOUT = 1, BT_PR_FAILED = 2, BT_PR_FULL = 3, BT_PR_CLOSED = 4 };
#define BT_SLOT_NONE 0xFF
#define BT_BATTERY_UNKNOWN 0xFF

static inline const char *bt_ipc_state_name(uint8_t s) {
    static const char *const n[] = {"starting", "ready", "no_adapter", "error"};
    return s < 4 ? n[s] : "?";
}

/* Writes one frame to out (needs BT_IPC_HDR + n bytes); returns its size, 0 when too long. */
static inline size_t bt_ipc_frame(uint8_t *out, uint8_t type, const uint8_t *p, size_t n) {
    if (n > BT_IPC_MAX_PAYLOAD) return 0;
    out[0] = type;
    out[1] = (uint8_t)(n >> 8);
    out[2] = (uint8_t)n;
    if (n) memcpy(out + BT_IPC_HDR, p, n);
    return BT_IPC_HDR + n;
}

/* Parses the frame at the start of buf[0..have): returns its size and sets type/payload/len, 0
 * when more bytes are needed, (size_t)-1 when the length is invalid (drop the connection). */
static inline size_t bt_ipc_parse(const uint8_t *buf, size_t have, uint8_t *type, const uint8_t **payload,
                                  size_t *len) {
    if (have < BT_IPC_HDR) return 0;
    size_t n = ((size_t)buf[1] << 8) | buf[2];
    if (n > BT_IPC_MAX_PAYLOAD) return (size_t)-1;
    if (have < BT_IPC_HDR + n) return 0;
    *type = buf[0];
    *payload = buf + BT_IPC_HDR;
    *len = n;
    return BT_IPC_HDR + n;
}

#endif
