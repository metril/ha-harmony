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
    uint8_t data[256]; /* 251 = the largest page that fits one 254-byte RF object */
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 2}, {.p = data, .len = 251}}, o[3];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), 0);
    CT_EQ(n, 254);
    CT_EQ(buf[2], 251);
    CT_EQ(hp_rf_unpack(HP_MSG_PAGE_PUSH, buf, n, o, 3), 0);
    CT_EQ(o[2].len, 251);
    CT_MEM_EQ(o[2].p, data, 251);
    v[2].len = 252; /* 255 bytes of payload no longer fit one object */
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), -HP_E_TOOBIG);
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

static void test_remote_info(void) {
    hp_rf_val_t v[7] = {{.u = 1}, {.u = 2}, {.u = 3}, {.u = 50}, {.u = 0xA7}, {.u = 12}, {.u = 0xBEEF}}, o[7];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_REMOTE_INFO, v, 7, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {1, 2, 3, 50, 0xA7, 12, 0xBE, 0xEF}; /* ..., gen of the stored v2 set, its page count, boot nonce */
    CT_EQ(n, sizeof want);
    CT_MEM_EQ(buf, want, sizeof want);
    CT_EQ(hp_rf_unpack(HP_MSG_REMOTE_INFO, buf, n, o, 7), 0);
    CT_EQ(o[1].u, 2);
    CT_EQ(o[3].u, 50);
    CT_EQ(o[4].u, 0xA7);
    CT_EQ(o[5].u, 12);
    CT_EQ(o[6].u, 0xBEEF);
    CT_CHECK(n <= HP_RF_CHUNK); /* the nonce still fits one RF block */
    CT_EQ(hp_rf_unpack(HP_MSG_REMOTE_INFO, buf, n - 1, o, 7), -HP_E_INVAL); /* truncated */
    buf[n] = 0;
    CT_EQ(hp_rf_unpack(HP_MSG_REMOTE_INFO, buf, n + 1, o, 7), -HP_E_INVAL); /* trailing */
    CT_EQ(hp_rf_unpack(HP_MSG_REMOTE_INFO, buf, 4, o, 7), -HP_E_INVAL); /* the 4-field form is gone */
}

static void test_tile_state(void) {
    const char *txt = "On";
    hp_rf_val_t v[5] = {{.u = 2}, {.u = 1}, {.u = 5}, {.u = 2}, {.p = (const uint8_t *)txt, .len = 2}}, o[5];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_TILE_STATE, v, 5, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {2, 1, 5, 2, 2, 'O', 'n'};
    CT_EQ(n, sizeof want);
    CT_MEM_EQ(buf, want, sizeof want);
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, buf, n, o, 5), 0);
    CT_EQ(o[3].u, 2);
    CT_EQ(o[4].len, 2);
    CT_MEM_EQ(o[4].p, txt, 2);
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, buf, n - 1, o, 5), -HP_E_INVAL); /* truncated text */
    /* the old 4-field tile_update shape is not a tile_state */
    CT_EQ(hp_rf_unpack(HP_MSG_TILE_STATE, buf, 4, o, 5), -HP_E_INVAL);
}

static void test_now_playing_and_cover_chunk(void) {
    hp_rf_val_t v[7] = {{.u = 1}, {.u = 0xDEADBEEF}, {.u = 1}, {.u = 65}, {.u = 3000}, {.p = (const uint8_t *)"Hi", .len = 2},
                        {.p = (const uint8_t *)"S1", .len = 2}}, o[7];
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n = 0;
    CT_EQ(hp_rf_pack(HP_MSG_NOW_PLAYING, v, 7, buf, sizeof buf, &n), 0);
    const uint8_t want[] = {1, 0xDE, 0xAD, 0xBE, 0xEF, 1, 0, 65, 0x0B, 0xB8, 2, 'H', 'i', 2, 'S', '1'};
    CT_EQ(n, sizeof want);
    CT_MEM_EQ(buf, want, sizeof want);
    CT_EQ(hp_rf_unpack(HP_MSG_NOW_PLAYING, buf, n, o, 7), 0);
    CT_EQ(o[1].u, 0xDEADBEEF);
    CT_EQ(o[4].u, 3000);
    CT_EQ(hp_rf_unpack(HP_MSG_NOW_PLAYING, buf, n - 1, o, 7), -HP_E_INVAL);
    uint8_t data[220];
    memset(data, 0xA5, sizeof data);
    hp_rf_val_t c[4] = {{.u = 1}, {.u = 7}, {.u = 1540}, {.p = data, .len = sizeof data}}, co[4];
    CT_EQ(hp_rf_pack(HP_MSG_COVER_CHUNK, c, 4, buf, sizeof buf, &n), 0);
    CT_EQ(n, 1 + 4 + 2 + 1 + 220);
    CT_EQ(hp_rf_unpack(HP_MSG_COVER_CHUNK, buf, n, co, 4), 0);
    CT_EQ(co[2].u, 1540);
    CT_EQ(co[3].len, 220);
    CT_EQ(hp_rf_unpack(HP_MSG_COVER_CHUNK, buf, n - 1, co, 4), -HP_E_INVAL);
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
    /* A 251-byte page = 254 payload bytes, the largest object body: 11 inner blocks. */
    uint8_t data[251] = {0};
    hp_rf_val_t v[3] = {{.u = 1}, {.u = 2}, {.p = data, .len = 251}};
    uint8_t big[255] = {0};
    hp_rf_val_t vb[3] = {{.u = 1}, {.u = 2}, {.p = big, .len = 255}};
    uint8_t buf[HP_RF_MSG_MAX];
    size_t n;
    hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
    size_t nf = 0;
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, v, 3, buf, sizeof buf, &n), 0);
    CT_EQ(n, HP_RF_BODY_MAX);
    CT_EQ(hp_rf_split(HP_MSG_PAGE_PUSH, 1, buf, n, fr, HP_RF_MAX_FRAGS, &nf), 0);
    CT_EQ(nf, 11);
    /* a 255-byte page (258) no longer fits one object */
    CT_EQ(hp_rf_pack(HP_MSG_PAGE_PUSH, vb, 3, buf, sizeof buf, &n), -HP_E_TOOBIG);
}

int main(void) {
    CT_RUN(test_key_event);
    CT_RUN(test_battery_big_endian);
    CT_RUN(test_state_update_bool);
    CT_RUN(test_page_push);
    CT_RUN(test_tile_update);
    CT_RUN(test_remote_info);
    CT_RUN(test_tile_state);
    CT_RUN(test_now_playing_and_cover_chunk);
    CT_RUN(test_rejects);
    CT_RUN(test_split_budget);
    return CT_DONE();
}
