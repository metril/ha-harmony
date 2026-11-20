#ifndef HP_NET_PEERS_H
#define HP_NET_PEERS_H

/* Remembered WS peers (IPv4) and the ARP keepalive.
 *
 * A peer on the hub's own L2 segment may be unable to ARP for the hub (its broadcast ARP requests can
 * go unanswered on the hub's Wi-Fi), but it learns the hub's MAC as soon as the hub sends it any unicast
 * frame. So the hub remembers the addresses of peers that authenticated, and when one loses its
 * authenticated connection (and once at boot, for all of them) it sends it a few small UDP datagrams to the
 * discard port with exponential back-off, then stops until the next such event (no periodic timer); the
 * kernel ARPs for the peer to send each, and the peer learns the hub's MAC from that request. A burst is
 * cancelled when the peer authenticates again.
 *
 * The list is persisted (newest first, one dotted quad per line, atomic write, only when the SET of
 * addresses changes). Addresses are host-order uint32. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_PEERS_MAX 64          /* ~1 KiB state file at most */
#define HP_PEERS_NUDGES 5        /* nudges per burst, at +1, +3, +7, +15, +31 s (gaps 1, 2, 4, 8, 16 s) */
#define HP_PEERS_FIRST_MS 1000   /* gap before the first nudge; doubles after each */

typedef struct {
    uint32_t addr;
    int64_t at_ms; /* when the next nudge is due */
    int step;      /* nudges already sent */
} hp_nudge_t;

typedef struct {
    uint32_t addr[HP_PEERS_MAX]; /* [0] is the most recent */
    int n;
    char path[256];              /* "" = not persisted */
    int64_t saved_ms;            /* when a reorder-only change was last persisted (0 = not yet this boot) */
    hp_nudge_t nudge[HP_PEERS_MAX]; /* active bursts */
    int nn;
} hp_peers_t;

/* Empty list; `path` may be NULL. */
void hp_peers_init(hp_peers_t *, const char *path);
/* Reads the file (missing/corrupt lines are skipped; never fails). Returns the number loaded. */
int hp_peers_load(hp_peers_t *);
/* Writes the file atomically. 0 or a negative -HP_E_*; no-op (0) without a path. */
int hp_peers_save(const hp_peers_t *);
#define HP_PEERS_REORDER_SAVE_MS (24LL * 3600 * 1000)
/* Records an authenticated peer at `now_ms` (monotonic): moves it to the front; evicts the oldest when
 * full. Returns true when the list should be persisted: a new address, or a known one that moved and no
 * save happened in the last 24 h (so the file order follows recent use without flash wear).
 * Unusable (non-private/loopback) addresses are ignored. */
bool hp_peers_note(hp_peers_t *, uint32_t addr, int64_t now_ms);
/* 10/8, 172.16/12, 192.168/16, 169.254/16 (not loopback, multicast, 0 or public). */
bool hp_peers_usable(uint32_t addr);
/* Starts a burst for a remembered peer (no-op for an unknown one or one already bursting). */
void hp_peers_arm(hp_peers_t *, uint32_t addr, int64_t now_ms);
/* Boot: starts a burst for every remembered peer. */
void hp_peers_arm_all(hp_peers_t *, int64_t now_ms);
/* The peer reconnected: stops its burst. */
void hp_peers_cancel(hp_peers_t *, uint32_t addr);
/* Writes up to `max` peers whose nudge is due at `now_ms` to out and schedules each one's next nudge (the
 * burst ends after HP_PEERS_NUDGES). Returns the count. */
int hp_peers_due(hp_peers_t *, int64_t now_ms, uint32_t *out, int max);
/* Peers (in list order, at most `max`) not in live[0..nlive). Returns the count written to out. */
int hp_peers_pending(const hp_peers_t *, const uint32_t *live, int nlive, uint32_t *out, int max);
/* One UDP datagram to addr:9, non-blocking, errors ignored. */
void hp_peers_poke(uint32_t addr);

#endif
