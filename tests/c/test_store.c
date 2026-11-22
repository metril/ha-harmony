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

/* HP_STORE_MAX = 252: ids 0xFC..0xFF belong to the UI's built-in screens (RA_UI_PAGE_*). */
static void test_total_over_the_max_is_refused(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    CT_EQ(HP_STORE_MAX, 252);
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 0, HP_STORE_MAX + 1, 1, 0, 0)), -HP_E_TOOBIG);
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, HP_STORE_MAX, 255, 1, 0, 0)), -HP_E_TOOBIG); /* id past the last slot */
    CT_EQ(hp_store_count(&S), 0u);
    CT_EQ(hp_store_put(&S, raw, mkpage(raw, 0, HP_STORE_MAX, 1, 0, 0)), HP_STORE_OK);
}

static void test_a_full_store_holds_every_id_up_to_the_last(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    for (unsigned i = 0; i < HP_STORE_MAX; i++)
        CT_CHECK(hp_store_put(&S, raw, mkpage(raw, (uint8_t)i, HP_STORE_MAX, 9, i == 0, 0)) >= 0);
    CT_EQ(hp_store_count(&S), (unsigned)HP_STORE_MAX);
    CT_EQ(hp_store_need(&S), (unsigned)HP_STORE_MAX);
    CT_CHECK(hp_store_complete(&S));
    hp_page_t p;
    CT_EQ(hp_store_get(&S, HP_STORE_MAX - 1, &p), 0); /* nav to the last id */
    CT_EQ(p.page_id, HP_STORE_MAX - 1);
    CT_CHECK(hp_store_ready(&S, HP_STORE_MAX - 1));
    CT_EQ(hp_store_start(&S), 0);
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

/* ---- the custom theme slot (theme_push): every ordering ---- */
static size_t mkthemed(uint8_t *o, uint8_t id, uint8_t total, uint8_t gen) {
    size_t n = mkpage(o, id, total, gen, 0, 0);
    o[7] = (uint8_t)(HP_PAGE_THEME_CUSTOM << 3); /* pflags: theme id 7 */
    return n;
}
static void theme_bytes(uint8_t *t, uint8_t seed) {
    memset(t, seed, HP_THEME_BYTES);
    t[0] = 1;
}

static void test_theme_then_pages(void) {
    hp_store_init(&S);
    uint8_t raw[40], t[HP_THEME_BYTES];
    theme_bytes(t, 9);
    CT_EQ(hp_store_theme(&S, 4, t, sizeof t), HP_STORE_OK);
    CT_CHECK(!hp_store_complete(&S));
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 2, 4)), HP_STORE_OK);
    CT_CHECK(!hp_store_complete(&S));
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 1, 2, 4)), HP_STORE_OK);
    CT_CHECK(hp_store_complete(&S) && hp_store_theme_ok(&S) && hp_store_ready(&S, 0));
}

static void test_pages_then_late_theme(void) {
    hp_store_init(&S);
    uint8_t raw[40], t[HP_THEME_BYTES];
    theme_bytes(t, 9);
    for (uint8_t i = 0; i < 2; i++) CT_EQ(hp_store_put(&S, raw, mkthemed(raw, i, 2, 4)), HP_STORE_OK);
    CT_EQ(hp_store_count(&S), 2u);
    CT_CHECK(!hp_store_complete(&S) && hp_store_theme_missing(&S) && !hp_store_ready(&S, 0));
    CT_EQ(hp_store_theme(&S, 4, t, sizeof t), HP_STORE_OK); /* the same gen: the pages stay */
    CT_CHECK(hp_store_complete(&S) && hp_store_ready(&S, 1));
}

static void test_a_preset_set_never_needs_the_slot(void) {
    hp_store_init(&S);
    uint8_t raw[40];
    for (uint8_t i = 0; i < 2; i++) CT_EQ(hp_store_put(&S, raw, mkpage(raw, i, 2, 4, 0, 0)), HP_STORE_OK);
    CT_CHECK(hp_store_complete(&S) && !hp_store_theme_missing(&S) && hp_store_ready(&S, 0));
}

static void test_theme_of_another_gen(void) {
    hp_store_init(&S);
    uint8_t raw[40], t[HP_THEME_BYTES];
    theme_bytes(t, 9);
    CT_EQ(hp_store_theme(&S, 4, t, sizeof t), HP_STORE_OK);
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 1, 4)), HP_STORE_OK);
    CT_CHECK(hp_store_complete(&S));
    theme_bytes(t, 7);
    CT_EQ(hp_store_theme(&S, 5, t, sizeof t), HP_STORE_CLEARED); /* a new gen: the set is stale */
    CT_EQ(hp_store_count(&S), 0u);
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 1, 4)), HP_STORE_OK); /* an old-gen page against the new slot */
    CT_CHECK(!hp_store_complete(&S) && !hp_store_ready(&S, 0));
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 1, 5)), HP_STORE_CLEARED);
    CT_CHECK(hp_store_complete(&S));
    /* a page of a newer gen replaces the set but the slot (gen 5) no longer matches */
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 1, 6)), HP_STORE_CLEARED);
    CT_CHECK(!hp_store_complete(&S) && S.theme_gen == 5);
}

static void test_duplicate_and_collision(void) {
    hp_store_init(&S);
    uint8_t raw[40], a[HP_THEME_BYTES], b[HP_THEME_BYTES];
    theme_bytes(a, 1);
    theme_bytes(b, 2);
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 1, 4)), HP_STORE_OK);
    CT_EQ(hp_store_theme(&S, 4, a, sizeof a), HP_STORE_OK);
    CT_EQ(hp_store_theme(&S, 4, a, sizeof a), HP_STORE_SAME); /* idempotent */
    CT_EQ(hp_store_theme(&S, 4, b, sizeof b), HP_STORE_OK);   /* same gen, other bytes: replaced, set kept */
    CT_CHECK(!memcmp(S.theme, b, sizeof b) && hp_store_complete(&S) && hp_store_count(&S) == 1u);
}

static void test_wrong_length_theme_is_refused(void) {
    hp_store_init(&S);
    uint8_t t[HP_THEME_BYTES] = {1};
    CT_EQ(hp_store_theme(&S, 4, t, HP_THEME_BYTES - 1), -HP_E_INVAL);
    CT_EQ(hp_store_theme(&S, 4, NULL, 0), -HP_E_INVAL);
    CT_CHECK(!S.have_theme);
}

static void test_a_replaced_set_keeps_the_slot(void) {
    hp_store_init(&S);
    uint8_t raw[40], t[HP_THEME_BYTES];
    theme_bytes(t, 3);
    CT_EQ(hp_store_theme(&S, 4, t, sizeof t), HP_STORE_OK);
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 2, 4)), HP_STORE_OK);
    CT_EQ(hp_store_put(&S, raw, mkthemed(raw, 0, 3, 4)), HP_STORE_CLEARED); /* a changed total: a new set, same gen */
    CT_CHECK(S.have_theme && S.theme_gen == 4);
    for (uint8_t i = 1; i < 3; i++) CT_EQ(hp_store_put(&S, raw, mkthemed(raw, i, 3, 4)), HP_STORE_OK);
    CT_CHECK(hp_store_complete(&S));
}

/* App status of a button: tile_state on a button id is kept in the overlay and lands in the decoded widget's tstate; a
 * re-put of the page drops it; a tile's own state is unaffected. */
static void test_button_app_status_overlay(void) {
    hp_store_init(&S);
    uint8_t raw[32];
    size_t n = mkpage(raw, 0, 1, 3, 0, 1);
    CT_EQ(hp_store_put(&S, raw, n), HP_STORE_OK);
    hp_page_t p;
    CT_EQ(hp_store_get(&S, 0, &p), 0);
    CT_EQ(p.widgets[0].kind, HP_WIDGET_BUTTON);
    CT_EQ(p.widgets[0].tstate, HP_APP_NONE);
    CT_EQ(hp_store_tile(&S, 0, p.widgets[0].id, HP_APP_PLAYING, "", 0), 0);
    CT_EQ(hp_store_get(&S, 0, &p), 0);
    CT_EQ(p.widgets[0].tstate, HP_APP_PLAYING);
    CT_EQ(hp_store_tile(&S, 0, p.widgets[0].id, HP_APP_PAUSED, "", 0), 0); /* the same id: replaced, not added */
    CT_EQ(hp_store_get(&S, 0, &p), 0);
    CT_EQ(p.widgets[0].tstate, HP_APP_PAUSED);
    CT_EQ(hp_store_tile(&S, 0, 99, HP_APP_OPEN, "", 0), 0); /* an id the page does not have: harmless */
    CT_EQ(hp_store_get(&S, 0, &p), 0);
    CT_EQ(p.widgets[0].tstate, HP_APP_PAUSED);
    CT_EQ(hp_store_put(&S, raw, n), HP_STORE_OK); /* the page is stored again: its overlay is dropped */
    CT_EQ(hp_store_get(&S, 0, &p), 0);
    CT_EQ(p.widgets[0].tstate, HP_APP_NONE);
}


int main(void) {
    CT_RUN(test_button_app_status_overlay);
    CT_RUN(test_theme_then_pages);
    CT_RUN(test_pages_then_late_theme);
    CT_RUN(test_a_preset_set_never_needs_the_slot);
    CT_RUN(test_theme_of_another_gen);
    CT_RUN(test_duplicate_and_collision);
    CT_RUN(test_wrong_length_theme_is_refused);
    CT_RUN(test_a_replaced_set_keeps_the_slot);
    CT_RUN(test_changed_total_with_same_gen_is_a_new_set);
    CT_RUN(test_total_over_the_max_is_refused);
    CT_RUN(test_a_full_store_holds_every_id_up_to_the_last);
    CT_RUN(test_lookups_never_return_an_id_past_total);
    return CT_DONE();
}
