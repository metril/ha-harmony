#ifndef HP_BT_LINK_H
#define HP_BT_LINK_H
/* Narrow interface between the hub's websocket handlers and the Bluetooth HID helper (docs/proto/bluetooth.md).
 *
 * NON-BLOCKING BY CONTRACT: every function here returns at once and is called from the hub's event-loop thread.
 * An implementation that needs to talk to a helper process queues the request and returns; the outcome arrives
 * later as a state change. Nothing here may sleep, wait on a socket or take a lock held across I/O.
 *
 * The listener (bt_link_set_listener) is invoked on the same thread, only from the event loop (never from inside
 * a bt_link_* call), once per observable state change; the hub then reads bt_link_state() and broadcasts bt_state.
 *
 * hub_handlers.c is the only caller. hub/bt_link_stub.c is the "no Bluetooth in this build" implementation. */
#include <stdbool.h>
#include <stdint.h>

#define BT_LINK_HOSTS_CAP 255 /* storage bound of struct bt_link_state; the real limit is bt_link_limits().hosts_max */
#define BT_LINK_REMOTES_CAP 32/* storage bound; the real limit is bt_link_limits().remotes_max */
#define BT_LINK_CANDIDATES_CAP 16 /* storage and wire bound of the candidate list */
#define BT_LINK_ADDR_MAX 18 /* "AA:BB:CC:DD:EE:FF" + NUL */
#define BT_LINK_NAME_MAX 49  /* UTF-8, at most 48 bytes + NUL; "" = no name */

#define BT_PAGE_KEYBOARD 7
#define BT_PAGE_CONSUMER 12

enum bt_link_status { BT_OFF, BT_STARTING, BT_READY, BT_NO_ADAPTER, BT_DISABLED };
enum bt_link_pair_result { BT_PAIR_NONE, BT_PAIR_OK, BT_PAIR_FAILED, BT_PAIR_REFUSED };
enum bt_link_action { BT_KEY_DOWN, BT_KEY_UP, BT_KEY_PRESS };
enum bt_link_pair_mode { BT_MODE_HOST, BT_MODE_REMOTE }; /* host: be discoverable; remote: scan for and bond an LE remote */
enum bt_link_kind { BT_KIND_HOST, BT_KIND_REMOTE };
enum bt_link_window { BT_WIN_NONE, BT_WIN_HOST, BT_WIN_REMOTE }; /* the pairing window that is open (or requested, helper not up yet) */
enum bt_link_transport { BT_TRANSPORT_ANY, BT_TRANSPORT_CLASSIC, BT_TRANSPORT_LE }; /* ANY: unknown (pairing) or "either" (request) */

/* Return codes of bt_link_pair / forget / key. */
#define BT_LINK_OK 0
#define BT_LINK_E_NOT_READY (-1)     /* adapter not ready (state is not "ready") */
#define BT_LINK_E_NO_SLOT (-2)       /* no host in that slot */
#define BT_LINK_E_NOT_CONNECTED (-3) /* host paired but not connected (bt_key) */
#define BT_LINK_E_BUSY (-4)          /* the helper cannot queue another request now */

struct bt_link_host {
    uint8_t slot;
    char addr[BT_LINK_ADDR_MAX];
    char name[BT_LINK_NAME_MAX]; /* "" = unknown */
    enum bt_link_transport transport; /* CLASSIC or LE */
    bool connected;
    bool configured; /* a config bt_hosts entry carries this address (slot is then the config slot); false = bonded in the helper only */
};

struct bt_link_remote {
    uint8_t slot;
    char addr[BT_LINK_ADDR_MAX];
    char name[BT_LINK_NAME_MAX];
    bool connected;
    int8_t battery; /* percent 0..100, -1 = unknown */
    bool configured; /* as bt_link_host */
};

/* An LE remote seen advertising while a remote-mode pairing window is open. */
struct bt_link_candidate {
    char addr[BT_LINK_ADDR_MAX];
    char name[BT_LINK_NAME_MAX]; /* "" = unknown */
    int8_t rssi;                 /* dBm */
};

struct bt_link_state {
    uint32_t gen; /* bumped by the link on every change (never by the discoverable_secs countdown) */
    enum bt_link_status status;
    bool discoverable;
    enum bt_link_window window;       /* BT_WIN_NONE = no pairing window */
    uint16_t discoverable_secs;       /* seconds left while discoverable, else 0 */
    bool le;                          /* the adapter can do Bluetooth LE */
    enum bt_link_pair_result pairing; /* last pairing outcome; BT_PAIR_NONE = nothing to report */
    char pairing_addr[BT_LINK_ADDR_MAX];
    char pairing_name[BT_LINK_NAME_MAX];
    enum bt_link_transport pairing_transport; /* ANY = not known */
    enum bt_link_kind pairing_kind;           /* what the last pairing outcome was for */
    uint8_t n_hosts;
    struct bt_link_host hosts[BT_LINK_HOSTS_CAP];
    uint8_t n_remotes;
    struct bt_link_remote remotes[BT_LINK_REMOTES_CAP];
    uint8_t n_candidates;         /* only while a remote-mode window is open, else 0 */
    struct bt_link_candidate candidates[BT_LINK_CANDIDATES_CAP];
};

/* Contract details (the implementation, not the hub, owns these):
 *  - bt_link_state() serves a cached snapshot; nothing here queries the helper synchronously.
 *  - If the helper dies, status leaves BT_READY (BT_OFF/BT_NO_ADAPTER), the cached hosts/remotes/pairing are cleared,
 *    `gen` is bumped and the listener fires.
 *  - BT_LINK_E_NO_SLOT / BT_LINK_E_NOT_CONNECTED are answered from the cache. A forget or key that the helper
 *    later fails is NOT reported back (bt_link_key is best effort).
 *  - A bt_link_pair/forget return of BT_LINK_OK means "queued": the hub replies with a snapshot taken at that
 *    moment, not the outcome. The outcome arrives as a later bt_state push.
 *  - The listener fires once per real change (it only sets a flag in the hub; one bt_state per event-loop pass).
 *    It must NOT fire for the discoverable_secs countdown: clients count down locally from the last value.
 *  - Candidates: while a remote-mode window is open the link scans and keeps up to BT_LINK_CANDIDATES_CAP advertising
 *    LE remotes (strongest first or first seen: its choice); the list is cleared when the window closes or the helper
 *    dies. The listener fires when the list's MEMBERSHIP changes (a device appears or drops out), NOT for an
 *    RSSI-only change; the hub reports whatever the cached snapshot holds.
 *  - A caller wakes the event loop itself when it changes state from another thread (the hub only polls its flag
 *    in hub_flush_pending). */
/* true when this build has Bluetooth (hello_ack carries `bt`, the bt_* requests are served). */
bool bt_link_available(void);
/* Open (on) or close the pairing window; secs is 1..300 and transport is one of the enum (already validated).
 * mode BT_MODE_HOST: become discoverable to a host; BT_MODE_REMOTE: scan for and bond an advertising LE remote
 * (transport is then ANY or LE; only offered when limits.remotes_max > 0 and limits.le). */
int bt_link_pair(bool on, unsigned secs, enum bt_link_transport transport, enum bt_link_pair_mode mode);
/* Bond the LE remote `addr` (AA:BB:CC:DD:EE:FF, exactly as listed in state.candidates; the hub has already checked it is
 * a current candidate). Non-blocking: BT_LINK_OK = queued; the outcome arrives as a later state push with `pairing`
 * (kind remote). Errors: BT_LINK_E_NOT_READY, BT_LINK_E_BUSY. */
int bt_link_pair_remote(const char *addr);
/* Remove the host (or bonded remote) in `slot`, disconnecting it; slot < hosts_max / remotes_max, already validated. */
int bt_link_forget(enum bt_link_kind kind, unsigned slot);
/* Send one HID usage (page 7 or 12, usage 0..0xFFFF and slot < hosts_max, already validated) to the host in `slot`. */
int bt_link_key(unsigned slot, unsigned page, unsigned usage, enum bt_link_action action);
/* Fixed limits of this build/adapter (valid slots are 0..hosts_max-1, hosts_max <= BT_LINK_HOSTS_CAP). */
struct bt_link_limits {
    uint8_t hosts_max; /* paired hosts kept */
    uint8_t conn_max;  /* simultaneous connections */
    uint16_t le_bonds_max; /* LE bonds the adapter/helper can store in all (hosts + remotes) */
    uint8_t remotes_max; /* bonded LE remotes kept (0 = no remote/input support), <= BT_LINK_REMOTES_CAP */
    bool le;           /* Bluetooth LE supported */
};
void bt_link_limits(struct bt_link_limits *out);
/* Snapshot of the current state; always fills *out (status BT_OFF, no hosts when unavailable). */
void bt_link_state(struct bt_link_state *out);
/* One listener; NULL clears it. */
void bt_link_set_listener(void (*cb)(void *ctx), void *ctx);
/* One key-from-a-bonded-remote callback (same thread rules as the listener); NULL clears it. The hub only
 * forwards it to clients that opted in with bt_listen; internal key handling is the link's own business. */
void bt_link_set_remote_key_cb(void (*cb)(void *ctx, unsigned slot, unsigned page, unsigned usage, bool down), void *ctx);

/* ---- Supervisor side (hub main loop and hub_handlers.c only; the stub and the test fake implement them as no-ops) ----
 * Slots: the config's bt_hosts / bt_remotes slot N is bound to the helper's bonded entry with the SAME ADDRESS (the helper's
 * slot table is its own). Hosts and remotes in the state carry the CONFIG slot when configured, else a free number that no
 * config entry uses; bt_link_key / forget / connect take those reported slots. A config host whose address is not bonded has
 * no entry: bt_link_key returns BT_LINK_E_NO_SLOT. */
struct bt_link_cfg_entry {
    uint8_t slot;
    char addr[BT_LINK_ADDR_MAX]; /* "" = the entry has no address (never bound) */
};
/* The loaded config's bt tables (call after every load; copies). The helper runs while any entry exists or a window is open. */
void bt_link_set_config(const struct bt_link_cfg_entry *hosts, unsigned n_hosts, const struct bt_link_cfg_entry *remotes,
                        unsigned n_remotes);
/* Where a configured remote's keys go; slots are config slots. All three run from bt_link_service. */
struct bt_link_engine {
    void *ctx;
    void (*remote_key)(void *ctx, unsigned slot, unsigned page, unsigned usage, bool down);
    void (*remote_lost)(void *ctx, unsigned slot); /* a connected configured remote disconnected (or the helper died) */
    void (*release_held)(void *ctx);               /* the helper died: nothing typed into a host stays down */
};
void bt_link_set_engine(const struct bt_link_engine *e);
/* An activity that uses the host in config slot `slot` started: connect it now. Best effort, non-blocking. */
void bt_link_connect(unsigned slot);
/* The helper's socket for the poll loop (-1 when no helper runs) and the periodic work: read, parse, ping, reap, respawn,
 * flush output, fire the listener. Returns the ms until it next wants to run (-1 = nothing scheduled). Never blocks. */
int bt_link_poll_fd(void);
/* Read end of the helper's stderr pipe (-1 none): poll it too; bt_link_service drains it into hp_log. */
int bt_link_log_fd(void);
int bt_link_service(void);

#endif
