#define _POSIX_C_SOURCE 200809L
#include "hub/net/peers.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hub/engine/atomic_file.h"
#include "hub/hp_errors.h"

void hp_peers_init(hp_peers_t *p, const char *path) {
    memset(p, 0, sizeof *p);
    if (path) snprintf(p->path, sizeof p->path, "%s", path);
}

bool hp_peers_usable(uint32_t a) {
    uint32_t b0 = a >> 24, b1 = (a >> 16) & 255;
    return b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168) || (b0 == 169 && b1 == 254);
}

bool hp_peers_note(hp_peers_t *p, uint32_t a, int64_t now_ms) {
    if (!hp_peers_usable(a)) return false;
    int i = 0;
    while (i < p->n && p->addr[i] != a) i++;
    bool added = i == p->n;
    if (added) {
        if (p->n < HP_PEERS_MAX) p->n++;
        i = p->n - 1; /* full: the oldest is overwritten */
    }
    bool moved = i > 0;
    memmove(&p->addr[1], &p->addr[0], (size_t)i * sizeof p->addr[0]);
    p->addr[0] = a;
    if (added || (moved && (p->saved_ms == 0 || now_ms - p->saved_ms >= HP_PEERS_REORDER_SAVE_MS))) {
        p->saved_ms = now_ms ? now_ms : 1;
        return true;
    }
    return false;
}

int hp_peers_load(hp_peers_t *p) {
    p->n = 0;
    if (!p->path[0]) return 0;
    FILE *f = fopen(p->path, "r");
    if (!f) return 0;
    char line[64];
    uint32_t tmp[HP_PEERS_MAX];
    int n = 0, lines = 0;
    while (lines++ < 4 * HP_PEERS_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        struct in_addr ia;
        if (inet_pton(AF_INET, line, &ia) != 1) continue;
        uint32_t a = ntohl(ia.s_addr);
        if (!hp_peers_usable(a)) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= tmp[i] == a;
        if (!dup && n < HP_PEERS_MAX) tmp[n++] = a;
    }
    fclose(f);
    memcpy(p->addr, tmp, (size_t)n * sizeof tmp[0]);
    p->n = n;
    return n;
}

int hp_peers_save(const hp_peers_t *p) {
    if (!p->path[0]) return 0;
    char buf[HP_PEERS_MAX * 16 + 1];
    size_t len = 0;
    for (int i = 0; i < p->n; i++) {
        struct in_addr ia = {htonl(p->addr[i])};
        char s[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &ia, s, sizeof s)) continue;
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", s);
    }
    return hp_atomic_write(p->path, (const uint8_t *)buf, len);
}

bool hp_peers_due(hp_peers_t *p, int64_t now_ms) {
    if (p->next_ms != 0 && now_ms < p->next_ms) return false;
    p->next_ms = now_ms + HP_PEERS_PERIOD_MS;
    return true;
}

int hp_peers_pending(const hp_peers_t *p, const uint32_t *live, int nlive, uint32_t *out, int max) {
    int k = 0;
    for (int i = 0; i < p->n && k < max; i++) {
        bool is_live = false;
        for (int j = 0; j < nlive; j++) is_live |= live[j] == p->addr[i];
        if (!is_live) out[k++] = p->addr[i];
    }
    return k;
}

void hp_peers_poke(uint32_t addr) {
    if (!hp_peers_usable(addr)) return;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    sa.sin_addr.s_addr = htonl(addr);
    (void)sendto(fd, "\0", 1, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
}
