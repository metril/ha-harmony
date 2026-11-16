#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/io/rf_pair.h"
#include "hub/io/sim_sinks.h"

static char path[108];

static void test_claim_single_owner(void) {
    hp_rf_transport *hub = NULL;
    CT_EQ(hp_rf_pair_create(path, &hub), 0);
    CT_EQ(hub->claim(hub->self), 0);
    CT_EQ(hub->claim(hub->self), -HP_E_BUSY);
    hp_rf_pair_destroy(hub);
    hp_sim_t sim; /* the scripted sim transport obeys the same rule */
    hp_sim_init(&sim, NULL);
    CT_EQ(sim.rf.claim(sim.rf.self), 0);
    CT_EQ(sim.rf.claim(sim.rf.self), -HP_E_BUSY);
}

static void claimed(hp_rf_transport *t) { CT_EQ(t->claim(t->self), 0); }

static void test_link(void) {
    hp_rf_transport *hub = NULL, *r1 = NULL, *r2 = NULL;
    uint8_t buf[40];
    const uint8_t f[] = {1, 2, 3, 4, 5};
    CT_EQ(hp_rf_pair_create(path, &hub), 0);
    claimed(hub);
    CT_EQ(hub->send(hub->self, f, sizeof f), -HP_E_NOENT); /* nobody attached */
    CT_EQ(hp_rf_pair_connect(path, &r1), 0);
    claimed(r1);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 0), 0); /* accepts */
    CT_CHECK(hp_rf_pair_connected(hub));
    CT_EQ(hp_rf_pair_wait_attached(r1, 1000), 0);
    CT_EQ(r1->send(r1->self, f, sizeof f), 0);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 1000), 5);
    CT_MEM_EQ(buf, f, 5);
    CT_EQ(hub->send(hub->self, f, 3), 0);
    CT_EQ(r1->recv(r1->self, buf, sizeof buf, 1000), 3);
    uint8_t big[33] = {0};
    CT_EQ(r1->send(r1->self, big, sizeof big), -HP_E_TOOBIG);

    /* second remote is refused while the first is attached */
    CT_EQ(hp_rf_pair_connect(path, &r2), 0);
    claimed(r2);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 0), 0);
    CT_EQ(hp_rf_pair_wait_attached(r2, 1000), -HP_E_BUSY);
    hp_rf_pair_destroy(r2);
    CT_EQ(hub->send(hub->self, f, 3), 0); /* first still attached */

    /* disconnect: queued frame first, then one NOENT, then a new remote can attach */
    CT_EQ(r1->send(r1->self, f, 2), 0);
    hp_rf_pair_destroy(r1);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 1000), 2);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 1000), -HP_E_NOENT);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 0), 0);
    CT_CHECK(!hp_rf_pair_connected(hub));
    CT_EQ(hp_rf_pair_connect(path, &r2), 0);
    claimed(r2);
    CT_EQ(hub->recv(hub->self, buf, sizeof buf, 0), 0);
    CT_EQ(hp_rf_pair_wait_attached(r2, 1000), 0);
    hp_rf_pair_destroy(r2);
    hp_rf_pair_destroy(hub);
}

int main(void) {
    snprintf(path, sizeof path, "/tmp/hp_rfpair_%d.sock", (int)getpid());
    CT_RUN(test_claim_single_owner);
    CT_RUN(test_link);
    unlink(path);
    return CT_DONE();
}
