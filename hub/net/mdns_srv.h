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

typedef struct {
    uint16_t port;     /* UDP port to bind (5353 normally; 0 = ephemeral, tests) */
    const char *addr;  /* NULL/multicast group: join it on INADDR_ANY. A unicast address
                        * (tests): bind just that, no multicast, replies are unicast. */
    const char *dest;  /* "a.b.c.d:port": where announcements/multicast replies go
                        * (default: the group on 5353; none for unicast-only mode). */
} hp_mdns_opts_t;

typedef struct {
    int fd; /* -1 when not running */
    hp_mdns_svc_t svc;
    hp_sched_t *sched;
    struct sockaddr_in dest;
    int have_dest, multicast;
    uint16_t port; /* actually bound */
    int announced;
    hp_sched_handle timer;
    char err[96];
} hp_mdns_srv_t;

/* 0 on success, -HP_E_* on failure (srv->err set, fd stays -1). */
int hp_mdns_srv_start(hp_mdns_srv_t *, hp_sched_t *, const hp_mdns_opts_t *, const char *hub_id,
                      uint16_t service_port, unsigned proto_v);
int hp_mdns_srv_fd(const hp_mdns_srv_t *);
uint16_t hp_mdns_srv_port(const hp_mdns_srv_t *);
/* Drains readable datagrams (bounded per call) and answers matching queries. */
void hp_mdns_srv_service(hp_mdns_srv_t *);
/* Sends the goodbye (TTL 0) records, cancels timers, closes the socket. Safe if not running. */
void hp_mdns_srv_stop(hp_mdns_srv_t *);

#endif
