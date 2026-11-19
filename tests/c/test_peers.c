#define _DEFAULT_SOURCE
/* Peer list (dedupe, order, cap, persistence, corrupt file) and the ARP keepalive scheduling decision. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/net/peers.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

static char dir[] = "/tmp/hub_peers_test_XXXXXX";

static void t_note(void) {
    hp_peers_t p;
    hp_peers_init(&p, NULL);
    CT_CHECK(hp_peers_note(&p, IP(192, 0, 2, 1)) == false); /* TEST-NET is not private: ignored */
    CT_CHECK(hp_peers_note(&p, IP(10, 0, 0, 1)));
    CT_CHECK(hp_peers_note(&p, IP(192, 168, 7, 2)));
    CT_CHECK(hp_peers_note(&p, IP(172, 16, 0, 3)));
    CT_CHECK(!hp_peers_note(&p, IP(10, 0, 0, 1))); /* dedupe: moves to front, set unchanged */
    CT_EQ(p.n, 3);
    CT_EQ(p.addr[0], IP(10, 0, 0, 1));
    CT_EQ(p.addr[1], IP(172, 16, 0, 3));
    CT_CHECK(!hp_peers_note(&p, IP(127, 0, 0, 1)));
    CT_CHECK(!hp_peers_note(&p, IP(8, 8, 8, 8)));
    CT_CHECK(!hp_peers_note(&p, 0));
    CT_CHECK(!hp_peers_note(&p, IP(172, 32, 0, 1)));
    CT_CHECK(hp_peers_note(&p, IP(169, 254, 1, 1)));
    CT_EQ(p.n, 4);
}

static void t_cap(void) {
    hp_peers_t p;
    hp_peers_init(&p, NULL);
    for (int i = 0; i < HP_PEERS_MAX + 10; i++) CT_CHECK(hp_peers_note(&p, IP(10, 1, i / 250, i % 250 + 1)));
    CT_EQ(p.n, HP_PEERS_MAX);
    CT_EQ(p.addr[0], IP(10, 1, (HP_PEERS_MAX + 9) / 250, (HP_PEERS_MAX + 9) % 250 + 1)); /* newest kept */
    CT_EQ(p.addr[HP_PEERS_MAX - 1], IP(10, 1, 0, 11)); /* the 10 oldest are gone */
}

static void t_persist(void) {
    char path[300];
    snprintf(path, sizeof path, "%s/peers", dir);
    hp_peers_t a, b;
    hp_peers_init(&a, path);
    CT_EQ(hp_peers_load(&a), 0); /* missing file */
    hp_peers_note(&a, IP(10, 0, 0, 5));
    hp_peers_note(&a, IP(192, 168, 1, 9));
    CT_EQ(hp_peers_save(&a), 0);
    hp_peers_init(&b, path);
    CT_EQ(hp_peers_load(&b), 2);
    CT_EQ(b.addr[0], IP(192, 168, 1, 9));
    CT_EQ(b.addr[1], IP(10, 0, 0, 5));
    /* corrupt: garbage, public, loopback, duplicate and overlong lines are skipped */
    FILE *f = fopen(path, "w");
    fputs("garbage\n8.8.8.8\n127.0.0.1\n10.0.0.7\n10.0.0.7\n\xff\xfe\n999.1.1.1\n", f);
    for (int i = 0; i < 200; i++) fputs("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", f);
    fputs("\n192.168.3.4", f); /* no trailing newline */
    fclose(f);
    hp_peers_init(&b, path);
    int n = hp_peers_load(&b);
    CT_CHECK(n >= 1 && n <= 2);
    CT_EQ(b.addr[0], IP(10, 0, 0, 7));
    f = fopen(path, "w"); /* empty file */
    fclose(f);
    CT_EQ(hp_peers_load(&b), 0);
    CT_EQ(b.n, 0);
    unlink(path);
}

static void t_sched(void) {
    hp_peers_t p;
    hp_peers_init(&p, NULL);
    CT_CHECK(hp_peers_due(&p, 1000)); /* first call: at once */
    CT_CHECK(!hp_peers_due(&p, 1001));
    CT_CHECK(!hp_peers_due(&p, 1000 + HP_PEERS_PERIOD_MS - 1));
    CT_CHECK(hp_peers_due(&p, 1000 + HP_PEERS_PERIOD_MS));
    CT_CHECK(!hp_peers_due(&p, 1000 + HP_PEERS_PERIOD_MS + 5));

    hp_peers_note(&p, IP(10, 0, 0, 1));
    hp_peers_note(&p, IP(10, 0, 0, 2));
    hp_peers_note(&p, IP(10, 0, 0, 3)); /* list: .3 .2 .1 */
    uint32_t out[8], live[2] = {IP(10, 0, 0, 2), IP(10, 9, 9, 9)};
    CT_EQ(hp_peers_pending(&p, live, 2, out, 8), 2); /* live peers are skipped, order kept */
    CT_EQ(out[0], IP(10, 0, 0, 3));
    CT_EQ(out[1], IP(10, 0, 0, 1));
    CT_EQ(hp_peers_pending(&p, NULL, 0, out, 8), 3);
    CT_EQ(hp_peers_pending(&p, NULL, 0, out, 1), 1);
    uint32_t all[3] = {IP(10, 0, 0, 1), IP(10, 0, 0, 2), IP(10, 0, 0, 3)};
    CT_EQ(hp_peers_pending(&p, all, 3, out, 8), 0);
    hp_peers_poke(IP(127, 0, 0, 1));      /* refused, no crash */
    hp_peers_poke(IP(10, 255, 255, 254)); /* send path runs; result ignored */
}

int main(void) {
    if (!mkdtemp(dir)) return 1;
    CT_RUN(t_note);
    CT_RUN(t_cap);
    CT_RUN(t_persist);
    CT_RUN(t_sched);
    rmdir(dir);
    return CT_DONE();
}
