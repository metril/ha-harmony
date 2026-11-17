/* Generated Elite key table (proto/schema.yaml keys:): id <-> evdev <-> name round trips. */
#include <string.h>

#include "ctest_assert.h"
#include "proto/gen/harmony_proto.h"

static void test_round_trip(void) {
    CT_EQ(HP_KEY_COUNT, 35);
    for (size_t i = 0; i < HP_KEY_COUNT; i++) {
        const hp_hwkey_t *k = &hp_hwkeys[i];
        CT_CHECK(k->id > 0 && k->id < HP_SOFT_KEY_BASE);
        CT_CHECK(hp_hwkey_by_id(k->id) == k);
        CT_CHECK(hp_hwkey_by_evdev(k->evdev) == k);
        CT_CHECK(hp_hwkey_by_name(k->name) == k);
        CT_CHECK(hp_hwkey_by_name(k->event) == k);
        CT_EQ(hp_key_from_evdev(k->evdev), k->id);
        for (size_t j = 0; j < i; j++) {
            CT_CHECK(hp_hwkeys[j].id != k->id && hp_hwkeys[j].evdev != k->evdev);
            CT_CHECK(strcmp(hp_hwkeys[j].name, k->name) != 0);
        }
    }
}

static void test_known_values(void) {
    CT_EQ(hp_hwkey_by_evdev(115)->id, hp_hwkey_by_name("VolumeUp")->id);
    CT_CHECK(!strcmp(hp_hwkey_by_evdev(364)->name, "Favorites"));
    CT_CHECK(!strcmp(hp_hwkey_by_name("HOME_CONTROL_1")->name, "HomeControl1"));
}

static void test_unknown(void) {
    CT_EQ(hp_key_from_evdev(9999), HP_KEY_NONE);
    CT_EQ(hp_key_from_evdev(0), HP_KEY_NONE);
    CT_CHECK(hp_hwkey_by_evdev(30) == NULL); /* KEY_A */
    CT_CHECK(hp_hwkey_by_id(0) == NULL && hp_hwkey_by_id(128) == NULL);
    CT_CHECK(hp_hwkey_by_name("num_0") == NULL && hp_hwkey_by_name("back") == NULL);
    CT_CHECK(hp_hwkey_by_name(NULL) == NULL);
}

int main(void) {
    CT_RUN(test_round_trip);
    CT_RUN(test_known_values);
    CT_RUN(test_unknown);
    return CT_DONE();
}
