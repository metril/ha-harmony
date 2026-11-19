#ifndef HP_NET_MDNS_H
#define HP_NET_MDNS_H

/* Minimal mDNS / DNS-SD responder core (RFC 6762 / 6763), pure functions only:
 * parse a query, build a response. No sockets here (see mdns_srv.h).
 * All wire access is explicit big-endian byte handling.
 *
 * Advertised: PTR _haharmony._tcp.local. -> <hub_id>._haharmony._tcp.local.,
 * the meta-query PTR _services._dns-sd._udp.local., SRV (-> <hub_id>.local.
 * :port), TXT (hub_id=<id>, v=<proto>[, hw=<hardware id>]) and one A record.
 *
 * NOT implemented: probing / conflict resolution, IPv6 / AAAA, multiple
 * interfaces, TC-bit multi-packet known-answer lists, SRV/TXT known-answer
 * suppression (PTR and A only), NSEC, name compression in responses,
 * the 20-120 ms random response delay. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_MDNS_LABEL_MAX 63
#define HP_MDNS_NAME_MAX 255

#define HP_MDNS_TTL_SERVICE 4500u /* PTR, TXT: 75 min (RFC 6762 s10) */
#define HP_MDNS_TTL_HOST 120u     /* SRV, A: 2 min */
#define HP_MDNS_TTL_LEGACY 10u    /* cap for legacy unicast replies (s6.7) */

enum { /* record sets, used both for "wanted by query" and "put in response" */
    HP_MDNS_PTR = 1,  /* _haharmony._tcp.local. PTR instance */
    HP_MDNS_ENUM = 2, /* _services._dns-sd._udp.local. PTR service */
    HP_MDNS_SRV = 4,
    HP_MDNS_TXT = 8,
    HP_MDNS_A = 16,
    HP_MDNS_ALL = 31
};
enum { HP_MDNS_F_GOODBYE = 1, HP_MDNS_F_LEGACY = 2 };

typedef struct {
    char hub_id[HP_MDNS_LABEL_MAX + 1];
    char inst[HP_MDNS_NAME_MAX + 1]; /* <hub_id>._haharmony._tcp.local. */
    char host[HP_MDNS_NAME_MAX + 1]; /* <hub_id>.local. */
    uint16_t port;
    uint32_t ipv4; /* host byte order */
    unsigned proto_v;
    char hw[32]; /* stable hardware id, advertised as TXT hw=<id> when not empty */
} hp_mdns_svc_t;

typedef struct {
    unsigned want; /* HP_MDNS_* answers the query asks for (after known-answer suppression) */
    bool unicast;  /* a matched question had the QU bit */
    uint16_t id;   /* query id (echoed in legacy replies) */
    bool have_q;   /* qname/qtype/qclass hold the first matched question */
    char qname[HP_MDNS_NAME_MAX + 1]; /* decompressed, original case, dotted, trailing '.' */
    uint16_t qtype, qclass;           /* qclass without the QU bit */
} hp_mdns_query_t;

/* 0, or -HP_E_INVAL for an empty/over-long/non-printable/dotted hub_id. */
int hp_mdns_svc_init(hp_mdns_svc_t *, const char *hub_id, uint16_t port, uint32_t ipv4_host, unsigned proto_v);

/* 0 for a well-formed standard query (want may be 0 = not for us),
 * -HP_E_INVAL for a response, truncated/hostile/garbage input. */
int hp_mdns_parse_query(const hp_mdns_svc_t *, const uint8_t *pkt, size_t len, hp_mdns_query_t *out);

/* Builds a response answering `answers` (HP_MDNS_*), plus related records as
 * additional. Returns bytes written or -HP_E_TOOBIG. GOODBYE: all TTLs 0.
 * LEGACY: id echoed, TTL capped to 10 s, no cache-flush bits. Otherwise id 0. */
int hp_mdns_build_response(const hp_mdns_svc_t *, unsigned answers, unsigned flags, uint16_t id,
                           uint8_t *out, size_t cap);
/* As above; with HP_MDNS_F_LEGACY and q->have_q the question is echoed
 * (QDCOUNT=1, RFC 6762 s6.7). q may be NULL. */
int hp_mdns_build_response_q(const hp_mdns_svc_t *, unsigned answers, unsigned flags, uint16_t id,
                             const hp_mdns_query_t *q, uint8_t *out, size_t cap);

#endif
