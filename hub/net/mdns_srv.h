#ifndef HP_NET_MDNS_SRV_H
#define HP_NET_MDNS_SRV_H

/* Socket layer for the mDNS responder (hub/net/mdns.h). Single-threaded and
 * non-blocking: the owner adds hp_mdns_srv_fd() to its poll set, calls
 * hp_mdns_srv_service() when it is readable, and the two startup
 * announcements (immediately, then 1 s later) run from the hp_sched timer
 * queue. Fails soft: start() returns a negative value and srv->err describes
 * why; the hub just runs without discovery. IPv4, one interface only. */

#include <stdint.h>
#include <netinet/in.h>

#include "hub/engine/sched.h"
#include "hub/net/mdns.h"

#define HP_MDNS_PORT 5353
#define HP_MDNS_GROUP "224.0.0.251"

#define HP_MDNS_RETRY_MIN_MS 1000u
#define HP_MDNS_RETRY_MAX_MS 60000u
#define HP_MDNS_RECHECK_MS 30000u
/* Next retry delay: doubles, capped at HP_MDNS_RETRY_MAX_MS. Pure. */
uint32_t hp_mdns_backoff_next(uint32_t cur_ms);
/* Finds the IPv4 (and netmask) used to reach the group; 0 or -1. Injectable for tests. */
typedef int (*hp_mdns_detect_fn)(uint32_t *ip_host, uint32_t *mask_host);

typedef struct {
    uint16_t port;     /* UDP port to bind (5353 normally; 0 = ephemeral, tests) */
    const char *addr;  /* NULL/multicast group: join it on INADDR_ANY. A unicast address
                        * (tests): bind just that, no multicast, replies are unicast. */
    const char *dest;  /* "a.b.c.d:port": where announcements/multicast replies go
                        * (default: the group on 5353; none for unicast-only mode). */
    hp_mdns_detect_fn detect; /* NULL = real interface detection (multicast mode only) */
    const char *hw;    /* optional stable hardware id for TXT hw= (NULL = none) */
} hp_mdns_opts_t;

/* Per-record-set multicast holdoff (RFC 6762 s6: one multicast of a record
 * per second). Stamps are valid only while (uint32_t)(now - then) < window. */
#define HP_MDNS_HOLDOFF_MS 1000u
typedef struct {
    uint32_t t[5]; /* last multicast time per record set (bit order PTR..A) */
    unsigned have; /* bitmask of record sets with a stamp */
} hp_mdns_rl_t;
void hp_mdns_rl_init(hp_mdns_rl_t *);
/* Returns the subset of `want` (HP_MDNS_*) allowed now and stamps it. */
unsigned hp_mdns_rl_take(hp_mdns_rl_t *, uint32_t now_ms, unsigned want);
/* Drops stamps older than the window so they can never alias after wraparound. */
void hp_mdns_rl_sweep(hp_mdns_rl_t *, uint32_t now_ms);

typedef struct {
    int fd; /* -1 when not running */
    int nl_fd; /* rtnetlink address-change socket (-1 = none: the address is rechecked on a timer instead) */
    hp_mdns_svc_t svc;
    hp_sched_t *sched;
    struct sockaddr_in dest;
    int have_dest, multicast;
    uint16_t port; /* actually bound */
    uint32_t mask; /* netmask of the mDNS interface (host order); 0xFFFFFFFF if unknown */
    int announced;
    hp_sched_handle timer;
    hp_mdns_rl_t rl;
    hp_mdns_detect_fn detect;
    struct in_addr bind_ip, group;
    uint16_t want_port;
    hp_sched_handle net_timer;
    uint32_t backoff;
    int logged; /* 0 none, 1 up, 2 down: one log line per change */
    char err[96];
} hp_mdns_srv_t;

/* True when `src` (host order) may be answered: same subnet as the receiving
 * interface (`if_addr`/`mask`) or loopback. Pure; anything else is off-link. */
int hp_mdns_on_link(uint32_t src, uint32_t if_addr, uint32_t mask);

/* 0 when configured (even if no address/socket yet: the daemon retries from
 * the scheduler with backoff, re-checks the address every 30 s and rebinds and
 * re-announces when it changes; fd is -1 meanwhile). -HP_E_* only for bad
 * options. Never blocks. */
int hp_mdns_srv_start(hp_mdns_srv_t *, hp_sched_t *, const hp_mdns_opts_t *, const char *hub_id,
                      uint16_t service_port, unsigned proto_v);
/* Wake-only fd: readable when an IPv4 address changed. The owner adds it to its poll set next to hp_mdns_srv_fd(); the
 * next hp_mdns_srv_service() handles it. -1 when there is none (no timer-free address watching then). */
int hp_mdns_srv_wake_fd(const hp_mdns_srv_t *);
int hp_mdns_srv_fd(const hp_mdns_srv_t *); /* may change over time; -1 when down */
uint16_t hp_mdns_srv_port(const hp_mdns_srv_t *);
/* Drains readable datagrams (bounded per call) and answers matching queries. */
void hp_mdns_srv_service(hp_mdns_srv_t *);
/* The hub's id changed: goodbye (TTL 0) for the old name, then announce under `hub_id` (the same port and
 * address). 0, or -HP_E_INVAL for an id that is not a DNS label (nothing changes). Safe when not running. */
int hp_mdns_srv_rename(hp_mdns_srv_t *, const char *hub_id);
/* Sends the goodbye (TTL 0) records, cancels timers, closes the socket. Safe if not running. */
void hp_mdns_srv_stop(hp_mdns_srv_t *);

#endif
