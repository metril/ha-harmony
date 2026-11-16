#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/rf/framing.h"
#include "hub/rf/rf_payload.h"

static void test_key_event(void) {
    hp_rf_val_t v[3] = {{.u = 7}, {.u = 0x42}, {.u = HP_KEY_ACTION_HOLD}}, o[3];
    uint8_t buf[8];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_KEY_EVENT, v, 3, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {7, 0x42, 2};
    CT_EQ(n, 3);
    CT_MEM_EQ(buf, want, 3);
    CT_EQ(hp_rf_unpack(HP_MSG_KEY_EVENT, buf, n, o, 3), 0);
    CT_EQ(o[0].u, 7);
    CT_EQ(o[1].u, 0x42);
    CT_EQ(o[2].u, 2);
    CT_EQ(hp_rf_unpack(HP_MSG_KEY_EVENT, buf, 2, o, 3), -HP_E_INVAL); /* truncated */
    buf[3] = 0;
    CT_EQ(hp_rf_unpack(HP_MSG_KEY_EVENT, buf, 4, o, 3), -HP_E_INVAL); /* trailing */
    buf[2] = 9;
    CT_EQ(hp_rf_unpack(HP_MSG_KEY_EVENT, buf, 3, o, 3), -HP_E_INVAL); /* bad enum */
}

static void test_battery_big_endian(void) {
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 88}, {.u = 0x0ED8}}, o[3];
    uint8_t buf[8];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_BATTERY, v, 3, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {1, 88, 0x0E, 0xD8};
    CT_EQ(n, 4);
    CT_MEM_EQ(buf, want, 4);
    CT_EQ(hp_rf_unpack(HP_MSG_BATTERY, buf, n, o, 3), 0);
    CT_EQ(o[2].u, 0x0ED8);
    v[1].u = 256; /* u8 overflow */
    CT_EQ(hp_rf_pack(HP_MSG_BATTERY, v, 3, buf, sizeof buf, &n), -HP_E_INVAL);
}

static void test_state_update_bool(void) {
    hp_rf_val_t v[2] = {{.u = 3}, {.u = 1}}, o[2];
    uint8_t buf[4];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_STATE_UPDATE, v, 2, buf, sizeof buf, &n), 0);
    CT_EQ(n, 2);
    CT_EQ(hp_rf_unpack(HP_MSG_STATE_UPDATE, buf, n, o, 2), 0);
    CT_EQ(o[1].u, 1);
    buf[1] = 2;
    CT_EQ(hp_rf_unpack(HP_MSG_STATE_UPDATE, buf, 2, o, 2), -HP_E_INVAL);
}

static void test_page_push(void) {
    uint8_t data[255];
    for (int i = 0; i < 255; i++) data[i] = (uint8_t)i;
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 2}, {.p = data, .len = 255}}, o[3];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), 0);
    CT_EQ(n, 258);
    CT_EQ(buf[2], 255);
    CT_EQ(hp_rf_unpack(HP_MSG_PAGE_PUSH, buf, n, o, 3), 0);
    CT_EQ(o[2].len, 255);
    CT_MEM_EQ(o[2].p, data, 255);
    v[2].len = 256; /* length prefix is u8 */
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), -HP_E_TOOBIG);
    v[2].len = 100;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, 50, &n), -HP_E_TOOBIG); /* out too small */
    CT_EQ(hp_rf_unpack(HP_MSG_PAGE_PUSH, buf, 10, o, 3), 0 + (-HP_E_INVAL)); /* len > remaining */
    /* empty data is fine */
    v[2].len = 0;
    v[2].p = NULL;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), 0);
    CT_EQ(n, 3);
}

static void test_tile_update(void) {
    const char *txt = "Vol 12";
    hp_rf_val_t v[4] = {{.u = 2}, {.u = 1}, {.u = 5}, {.p = (const uint8_t *)txt, .len = 6}}, o[4];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_TILE_UPDATE, v, 4, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {2, 1, 5, 6, 'V', 'o', 'l', ' ', '1', '2'};
    CT_EQ(n, sizeof want);
    CT_MEM_EQ(buf, want, sizeof want);
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_UPDATE, buf, n, o, 4), 0);
    CT_EQ(o[2].u, 5);
    CT_EQ(o[3].len, 6);
    CT_MEM_EQ(o[3].p, txt, 6);
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_UPDATE, buf, n - 1, o, 4), -HP_E_INVAL); /* truncated text */
    hp_rf_val_t w[3] = {{.p = (const uint8_t *)"tv", .len = 2}, {.u = 1}, {.p = NULL, .len = 0}};
    CT_EQ(hp_rf_pack(HP_MSG_DEVICE_STATE, w, 3, buf, sizeof buf, &n), -HP_E_INVAL); /* ws-only */
}

static void test_rejects(void) {
    hp_rf_val_t v[3] = {{0}};
    uint8_t buf[8];
    size_t n;
    CT_EQ(hp_rf_pack(HP_MSG_HELLO, v, 2, buf, sizeof buf, &n), -HP_E_INVAL);       /* ws-only */
    CT_EQ(hp_rf_pack(HP_MSG_SET_CONFIG, v, 1, buf, sizeof buf, &n), -HP_E_INVAL);  /* json */
    CT_EQ(hp_rf_pack(HP_MSG_KEY_EVENT, v, 2, buf, sizeof buf, &n), -HP_E_INVAL);   /* wrong nvals */
    CT_EQ(hp_rf_pack((hp_msg_type_t)99, v, 3, buf, sizeof buf, &n), -HP_E_INVAL);
}

static void test_split_budget(void) {
    /* 255-byte page = 258 payload bytes: 10 fragments at the hub->remote cap of 30. */
    uint8_t data[255] = {0};
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 2}, {.p = data, .len = 255}};
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n;
    hp_rf_frame_t fr[HP_RF_MAX_FRAGS];
    size_t nf = 0;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), 0);
    CT_EQ(hp_rf_split(HP_MSG_PAGE_PUSH, 1, buf, n, 30, fr, HP_RF_MAX_FRAGS, &nf), 0);
    CT_EQ(nf, 10);
}

int main(void) {
    CT_RUN(test_key_event);
    CT_RUN(test_battery_big_endian);
    CT_RUN(test_state_update_bool);
    CT_RUN(test_page_push);
    CT_RUN(test_tile_update);
    CT_RUN(test_rejects);
    CT_RUN(test_split_budget);
    return CT_DONE();
}
