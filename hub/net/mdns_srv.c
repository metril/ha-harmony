#define _DEFAULT_SOURCE /* getifaddrs / IFF_* on glibc and uClibc */
#define _POSIX_C_SOURCE 200809L
#include "hub/net/hublog.h"
#include "hub/net/mdns_srv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hub/hp_errors.h"

#define ANNOUNCE_COUNT 2
#define ANNOUNCE_GAP_MS 1000
#define MAX_PER_SERVICE 8
#define PKT_CAP 1500

static int fail(hp_mdns_srv_t *s, const char *what) {
    snprintf(s->err, sizeof s->err, "%s: %s", what, strerror(errno));
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    return -HP_E_IO;
}

static int is_mcast(uint32_t host_ip) { return (host_ip >> 28) == 0xE; }

/* The IPv4 we would use to reach the group (connected-UDP trick: no packet is
 * sent), else the first up, non-loopback, multicast-capable interface. */
static int detect_ip(uint32_t *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in g, me;
        socklen_t ml = sizeof me;
        memset(&g, 0, sizeof g);
        g.sin_family = AF_INET;
        g.sin_port = htons(HP_MDNS_PORT);
        inet_pton(AF_INET, HP_MDNS_GROUP, &g.sin_addr);
        if (connect(fd, (struct sockaddr *)&g, sizeof g) == 0 &&
            getsockname(fd, (struct sockaddr *)&me, &ml) == 0) {
            uint32_t ip = ntohl(me.sin_addr.s_addr);
            if (ip && (ip >> 24) != 127) { close(fd); *out = ip; return 0; }
        }
        close(fd);
    }
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) < 0) return -1;
    int rc = -1;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_MULTICAST)) continue;
        *out = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        rc = 0;
        break;
    }
    freeifaddrs(ifa);
    return rc;
}

int hp_mdns_on_link(uint32_t src, uint32_t if_addr, uint32_t mask) {
    if ((src >> 24) == 127) return 1;
    return src != 0 && (src & mask) == (if_addr & mask);
}

/* Netmask (host order) of the interface owning `ip`; all-ones (fail closed) if not found. */
static uint32_t find_mask(uint32_t ip) {
    uint32_t m = 0xFFFFFFFFu;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) < 0) return m;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET) continue;
        if (ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr) != ip) continue;
        m = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
        break;
    }
    freeifaddrs(ifa);
    return m;
}

static int parse_dest(const char *s, struct sockaddr_in *out) {
    char ip[32];
    const char *c = strchr(s, ':');
    if (!c || (size_t)(c - s) >= sizeof ip) return -1;
    memcpy(ip, s, (size_t)(c - s));
    ip[c - s] = 0;
    char *end;
    long port = strtol(c + 1, &end, 10);
    if (!c[1] || *end || port < 1 || port > 65535) return -1;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &out->sin_addr) == 1 ? 0 : -1;
}

void hp_mdns_rl_init(hp_mdns_rl_t *r) { memset(r, 0, sizeof *r); }

unsigned hp_mdns_rl_take(hp_mdns_rl_t *r, uint32_t now, unsigned want) {
    unsigned ok = 0;
    for (unsigned i = 0; i < 5; i++) {
        unsigned b = 1u << i;
        if (!(want & b)) continue;
        if ((r->have & b) && (uint32_t)(now - r->t[i]) < HP_MDNS_HOLDOFF_MS) continue;
        r->t[i] = now;
        r->have |= b;
        ok |= b;
    }
    return ok;
}

void hp_mdns_rl_sweep(hp_mdns_rl_t *r, uint32_t now) {
    for (unsigned i = 0; i < 5; i++)
        if ((r->have & (1u << i)) && (uint32_t)(now - r->t[i]) >= HP_MDNS_HOLDOFF_MS) r->have &= ~(1u << i);
}

static void send_to(hp_mdns_srv_t *s, const struct sockaddr_in *to, unsigned answers, unsigned flags,
                    uint16_t id, const hp_mdns_query_t *q) {
    uint8_t buf[PKT_CAP];
    int n = hp_mdns_build_response_q(&s->svc, answers, flags, id, q, buf, sizeof buf);
    if (n > 0) (void)sendto(s->fd, buf, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void announce_cb(void *arg, uint32_t now) {
    hp_mdns_srv_t *s = arg;
    s->timer = 0;
    (void)hp_mdns_rl_take(&s->rl, now, HP_MDNS_ALL); /* announcements count as multicasts */
    if (s->have_dest) send_to(s, &s->dest, HP_MDNS_ALL, 0, 0, NULL);
    if (++s->announced < ANNOUNCE_COUNT) s->timer = hp_sched_after(s->sched, ANNOUNCE_GAP_MS, announce_cb, s);
}

uint32_t hp_mdns_backoff_next(uint32_t cur) {
    if (cur >= HP_MDNS_RETRY_MAX_MS / 2) return HP_MDNS_RETRY_MAX_MS;
    return cur < HP_MDNS_RETRY_MIN_MS ? HP_MDNS_RETRY_MIN_MS : cur * 2;
}

static int detect_default(uint32_t *ip, uint32_t *mask) {
    if (detect_ip(ip) < 0) return -1;
    *mask = find_mask(*ip);
    return 0;
}

static void log_state(hp_mdns_srv_t *s, int st) {
    if (s->logged == st) return;
    int first = s->logged == 0;
    s->logged = st;
    if (st == 2) {
        hp_log("hub: mdns disabled (%s); retrying\n", s->err);
    } else if (!first) { /* first-time success is reported by the owner (port line) */
        hp_log("hub: mdns enabled ip=%u.%u.%u.%u port=%u\n", (unsigned)(s->svc.ipv4 >> 24),
                (unsigned)((s->svc.ipv4 >> 16) & 255), (unsigned)((s->svc.ipv4 >> 8) & 255),
                (unsigned)(s->svc.ipv4 & 255), (unsigned)s->port);
    }
}

/* Opens, binds and joins on `ip`. 0 or -HP_E_IO (s->err set, fd -1). */
static int attach(hp_mdns_srv_t *s, uint32_t ip, uint32_t mask) {
    s->svc.ipv4 = ip;
    s->mask = mask;
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) return fail(s, "socket");
    int one = 1;
    if (setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) return fail(s, "SO_REUSEADDR");
#ifdef SO_REUSEPORT
    if (s->multicast) (void)setsockopt(s->fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in b;
    memset(&b, 0, sizeof b);
    b.sin_family = AF_INET;
    b.sin_port = htons(s->port ? s->port : s->want_port); /* keep the port across rebinds */
    b.sin_addr = s->bind_ip;
    if (bind(s->fd, (struct sockaddr *)&b, sizeof b) < 0) return fail(s, "bind");
    if (s->multicast) {
        struct ip_mreq m;
        m.imr_multiaddr = s->group;
        m.imr_interface.s_addr = htonl(ip);
        if (setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) return fail(s, "join group");
        struct in_addr ifa;
        ifa.s_addr = htonl(ip);
        (void)setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
        unsigned char ttl = 255; /* RFC 6762 s11 */
        (void)setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    socklen_t bl = sizeof b;
    if (getsockname(s->fd, (struct sockaddr *)&b, &bl) < 0) return fail(s, "getsockname");
    s->port = ntohs(b.sin_port);
    int fl = fcntl(s->fd, F_GETFL, 0);
    if (fl < 0 || fcntl(s->fd, F_SETFL, fl | O_NONBLOCK) < 0) return fail(s, "O_NONBLOCK");
    (void)fcntl(s->fd, F_SETFD, FD_CLOEXEC);
    s->announced = 0;
    s->timer = hp_sched_after(s->sched, 0, announce_cb, s);
    return 0;
}

static int probe(hp_mdns_srv_t *s, uint32_t *ip, uint32_t *mask) {
    if (!s->multicast) {
        *ip = ntohl(s->bind_ip.s_addr);
        *mask = find_mask(*ip);
        return 0;
    }
    return s->detect(ip, mask);
}

static void net_cb(void *arg, uint32_t now);

/* One connect attempt; schedules the next check (recheck when up, backoff when not). */
static void net_try(hp_mdns_srv_t *s, uint32_t now) {
    uint32_t ip = 0, mask = 0;
    s->net_timer = 0;
    hp_mdns_rl_sweep(&s->rl, now); /* no stale stamp survives to alias after wraparound */
    int ok = probe(s, &ip, &mask) == 0;
    if (s->fd >= 0) {
        if (ok && ip == s->svc.ipv4) { /* unchanged */
            s->net_timer = hp_sched_after(s->sched, HP_MDNS_RECHECK_MS, net_cb, s);
            return;
        }
        if (s->timer) hp_sched_cancel(s->sched, s->timer);
        s->timer = 0;
        close(s->fd); /* address changed or lost: leave the old membership */
        s->fd = -1;
        s->backoff = HP_MDNS_RETRY_MIN_MS;
    }
    if (!ok) snprintf(s->err, sizeof s->err, "no usable IPv4 interface");
    if (ok && attach(s, ip, mask) == 0) {
        s->backoff = HP_MDNS_RETRY_MIN_MS;
        log_state(s, 1);
        s->net_timer = hp_sched_after(s->sched, HP_MDNS_RECHECK_MS, net_cb, s);
        return;
    }
    log_state(s, 2);
    s->net_timer = hp_sched_after(s->sched, s->backoff, net_cb, s);
    s->backoff = hp_mdns_backoff_next(s->backoff);
}

static void net_cb(void *arg, uint32_t now) { net_try(arg, now); }

int hp_mdns_srv_start(hp_mdns_srv_t *s, hp_sched_t *sched, const hp_mdns_opts_t *o, const char *hub_id,
                      uint16_t service_port, unsigned proto_v) {
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->sched = sched;
    s->want_port = o->port;
    s->detect = o->detect ? o->detect : detect_default;
    s->backoff = HP_MDNS_RETRY_MIN_MS;
    inet_pton(AF_INET, HP_MDNS_GROUP, &s->group);
    s->multicast = 1;
    s->bind_ip.s_addr = htonl(INADDR_ANY);
    if (o->addr) {
        struct in_addr a;
        if (inet_pton(AF_INET, o->addr, &a) != 1) {
            snprintf(s->err, sizeof s->err, "bad --mdns-addr %s", o->addr);
            return -HP_E_INVAL;
        }
        if (is_mcast(ntohl(a.s_addr))) s->group = a;
        else { s->multicast = 0; s->bind_ip = a; }
    }
    if (hp_mdns_svc_init(&s->svc, hub_id, service_port, 0, proto_v) < 0) {
        snprintf(s->err, sizeof s->err, "invalid hub id for DNS label");
        return -HP_E_INVAL;
    }
    if (o->dest) {
        if (parse_dest(o->dest, &s->dest) < 0) {
            snprintf(s->err, sizeof s->err, "bad --mdns-dest %s", o->dest);
            return -HP_E_INVAL;
        }
        s->have_dest = 1;
    } else if (s->multicast) {
        memset(&s->dest, 0, sizeof s->dest);
        s->dest.sin_family = AF_INET;
        s->dest.sin_port = htons(HP_MDNS_PORT);
        s->dest.sin_addr = s->group;
        s->have_dest = 1;
    }
    if (o->hw && strlen(o->hw) < sizeof s->svc.hw) snprintf(s->svc.hw, sizeof s->svc.hw, "%s", o->hw);
    net_try(s, hp_sched_now(sched)); /* first attempt now; failures retry from the scheduler */
    return 0;
}

int hp_mdns_srv_rename(hp_mdns_srv_t *s, const char *hub_id) {
    hp_mdns_svc_t n;
    if (hp_mdns_svc_init(&n, hub_id, s->svc.port, s->svc.ipv4, s->svc.proto_v) < 0) return -HP_E_INVAL;
    snprintf(n.hw, sizeof n.hw, "%s", s->svc.hw);
    if (s->fd >= 0 && s->have_dest) send_to(s, &s->dest, HP_MDNS_ALL, HP_MDNS_F_GOODBYE, 0, NULL); /* old instance and host */
    s->svc = n;
    if (s->fd >= 0) {
        if (s->timer) hp_sched_cancel(s->sched, s->timer);
        hp_mdns_rl_init(&s->rl);
        s->announced = 0;
        s->timer = hp_sched_after(s->sched, 0, announce_cb, s);
    }
    return 0;
}

int hp_mdns_srv_fd(const hp_mdns_srv_t *s) { return s->fd; }
uint16_t hp_mdns_srv_port(const hp_mdns_srv_t *s) { return s->port; }

void hp_mdns_srv_service(hp_mdns_srv_t *s) {
    if (s->fd < 0) return;
    for (int i = 0; i < MAX_PER_SERVICE; i++) {
        uint8_t buf[PKT_CAP];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return; /* EAGAIN / EINTR / error: nothing more now */
        /* Off-link sources (spoofed reflection victims) get nothing, RFC 6762 s11. */
        if (fl < sizeof from || from.sin_family != AF_INET ||
            !hp_mdns_on_link(ntohl(from.sin_addr.s_addr), s->svc.ipv4, s->mask))
            continue;
        hp_mdns_query_t q;
        if (hp_mdns_parse_query(&s->svc, buf, (size_t)n, &q) < 0 || !q.want) continue;
        int legacy = ntohs(from.sin_port) != HP_MDNS_PORT; /* RFC 6762 s6.7 */
        unsigned flags = legacy ? HP_MDNS_F_LEGACY : 0;
        if (legacy || q.unicast || !s->multicast || !s->have_dest) send_to(s, &from, q.want, flags, q.id, &q);
        else {
            unsigned w = hp_mdns_rl_take(&s->rl, hp_sched_now(s->sched), q.want);
            if (w) send_to(s, &s->dest, w, 0, 0, NULL);
        }
    }
}

void hp_mdns_srv_stop(hp_mdns_srv_t *s) {
    if (!s->sched) return;
    if (s->timer) hp_sched_cancel(s->sched, s->timer);
    if (s->net_timer) hp_sched_cancel(s->sched, s->net_timer);
    s->timer = s->net_timer = 0;
    if (s->fd < 0) return;
    if (s->have_dest) send_to(s, &s->dest, HP_MDNS_ALL, HP_MDNS_F_GOODBYE, 0, NULL);
    close(s->fd);
    s->fd = -1;
}
