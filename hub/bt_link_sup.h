#ifndef HP_BT_LINK_SUP_H
#define HP_BT_LINK_SUP_H
/* Configuration and test access to the real bt_link (hub/bt_link.c, HH_BT builds only). main.c sets the helper
 * location and data directory; the tests replace the clock, the stock-bluetoothd stop and the helper executable. */
#include <stdint.h>

struct bt_link_hooks {
    uint64_t (*now_ms)(void);       /* NULL = CLOCK_MONOTONIC */
    /* Sends `sig` (0 = only probe) to every stock `bluetoothd` process; returns how many exist. NULL = scan /proc. */
    int (*bluetoothd_signal)(int sig);
    const char *helper_exe;         /* NULL = /proc/self/exe, falling back to argv0 */
    const char *argv0;
    const char *data_dir;           /* keys live in <data_dir>/bt/keys.tlv; NULL = /data/ha-harmony */
    unsigned helper_hci;            /* tests only: passed as `--hci N` when nonzero (never run the real helper without it) */
    int skip_bluetoothd;            /* --sim: never touch the stock daemon */
    int unavailable;                /* --sim without --bt-helper: behave as a hub without Bluetooth (bt_link_available() false) */
};
void bt_link_configure(const struct bt_link_hooks *);
/* Finds the stock `bluetoothd` among the entries of a /proc-like directory (the name from comm, else stat, else cmdline; the
 * hub's kernel has no comm). Stores up to max pids, returns how many exist; `self` is skipped. */
int bt_link_find_bluetoothd(const char *proc_root, long self, long *pids, int max);

/* Counters for diagnostics and tests. */
struct bt_link_stats {
    unsigned key_drops;  /* KEY frames dropped because the socket was full */
    unsigned deaths;     /* unexpected helper exits/kills this boot (the respawn budget counts these) */
    unsigned spawns;
    unsigned bad_frames; /* helper frames that were malformed (each one kills the helper) */
    int pid;             /* running helper, 0 = none */
};
void bt_link_stats(struct bt_link_stats *out);
/* Tests: kill any helper, forget everything (config, cache, counters, hooks). */
void bt_link_reset(void);

#define BT_SUP_PING_MS 5000
#define BT_SUP_SILENCE_MS 15000
#define BT_SUP_MAX_RESPAWNS 5
#define BT_SUP_LINGER_MS 60000 /* the helper keeps running this long after a pairing window closed with no config entries */

#endif
