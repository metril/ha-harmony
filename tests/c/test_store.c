/* The remote's page store: gen / total collisions, size cap, and bounded kind / act lookups. */
#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "remote/page/page.h"
#include "remote/page/store.h"

static size_t mkpage(uint8_t *o, uint8_t id, uint8_t total, uint8_t gen, uint8_t kind, uint8_t act) {
    const uint8_t b[] = {2, id, 2, 2, 1, gen, total, kind, 0, act, 0, 0, 1, 0x00, 0x11, 1, 'X'};
    memcpy(o, b, sizeof b);
    return sizeof b;
}

static hp_store_t S;

static void test_changed_total_with_same_gen_is_a_new_set(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    for (uint8_t i = 0; i < 3; i++) CT_EQ(hp_store_put(&S, raw, mkpage(raw, i, 3, 7, i == 1, 0)), HP_STORE_OK);
    CT_CHECK(hp_store_complete(&S));
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 0, 5, 7, 0, 0)), HP_STORE_CLEARED); /* same gen, other total */
    CT_EQ(hp_store_count(&S), 1u);
    CT_CHECK(!hp_store_complete(&S));
    CT_EQ(hp_store_need(&S), 5u);
    CT_EQ(hp_store_find_kind(&S, HP_PAGE_KIND_HOME), -1); /* the old home is gone */
}

static void test_total_over_32_is_refused(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 0, 33, 1, 0, 0)), -HP_E_TOOBIG);
    CT_EQ(hp_store_count(&S), 0u);
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 0, 32, 1, 0, 0)), HP_STORE_OK);
}

static void test_lookups_never_return_an_id_past_total(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 9, 10, 1, HP_PAGE_KIND_HOME, 3)), HP_STORE_OK);
    S.total = 4; /* a corrupted / racing total: lookups must still stay inside it */
    CT_EQ(hp_store_find_kind(&S, HP_PAGE_KIND_HOME), -1);
    CT_EQ(hp_store_find_act(&S, 3), -1);
    CT_EQ(hp_store_start(&S), -1);
}

int main(void) {
    CT_RUN(test_changed_total_with_same_gen_is_a_new_set);
    CT_RUN(test_total_over_32_is_refused);
    CT_RUN(test_lookups_never_return_an_id_past_total);
    return CT_DONE();
}
