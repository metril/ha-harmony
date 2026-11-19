/* Bluetooth helper pure logic: HID report building, remote key diff, IPC framing. */
#include <string.h>

#include "ctest_assert.h"
#include "hub/bt/bt_hid.h"
#include "hub/bt/bt_ipc.h"
#include "hub/bt/bt_remote.h"

static void test_keyboard_reports(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    CT_EQ(bt_hid_apply(&k, 0x07, 0x04, true, r), 10); /* A down */
    CT_EQ(r[0], 0xA1);
    CT_EQ(r[1], 1);
    CT_EQ(r[4], 0x04);
    CT_EQ(bt_hid_apply(&k, 0x07, 0xE1, true, r), 10); /* left shift joins */
    CT_EQ(r[2], 0x02);
    CT_EQ(r[4], 0x04);
    CT_EQ(bt_hid_apply(&k, 0x07, 0x04, false, r), 10);
    CT_EQ(r[4], 0);
    CT_EQ(r[2], 0x02);
    CT_EQ(bt_hid_apply(&k, 0x07, 0xE1, false, r), 10);
    for (int i = 2; i < 10; i++) CT_EQ(r[i], 0); /* all released */
    CT_EQ(bt_hid_apply(&k, 0x07, 0, true, r), 0);
    CT_EQ(bt_hid_apply(&k, 0x08, 1, true, r), 0); /* unknown page */
}

static void test_six_key_rollover(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    for (uint16_t u = 4; u < 11; u++) bt_hid_apply(&k, 0x07, u, true, r); /* 7 keys */
    int n = 0;
    for (int i = 4; i < 10; i++) n += r[i] != 0;
    CT_EQ(n, 6);
}

static void test_consumer(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x00E9, true, r), 4); /* volume up */
    CT_EQ(r[1], 2);
    CT_EQ(r[2], 0xE9);
    CT_EQ(r[3], 0x00);
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x00E9, false, r), 4);
    CT_EQ(r[2] | r[3], 0);
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x0400, true, r), 0); /* beyond the descriptor's range */
}

static int s_events[8][3];
static int s_n;
static void emit(uint8_t page, uint16_t usage, bool down, void *ctx) {
    (void)ctx;
    s_events[s_n][0] = page;
    s_events[s_n][1] = usage;
    s_events[s_n][2] = down;
    s_n++;
}

static void test_remote_diff(void) {
    bt_rkey_set_t old = {.n = 0}, now = {.n = 0};
    now.act[now.n++] = (bt_rkey_t){0x0C, 0x42};
    bt_rkey_diff(&old, &now, emit, NULL);
    CT_EQ(s_n, 1);
    CT_EQ(s_events[0][2], 1);
    now.n = 0;
    now.act[now.n++] = (bt_rkey_t){0x07, 0x28};
    bt_rkey_diff(&old, &now, emit, NULL); /* one key replaced by another */
    CT_EQ(s_n, 3);
    CT_EQ(s_events[1][2], 0);
    CT_EQ(s_events[1][1], 0x42);
    CT_EQ(s_events[2][1], 0x28);
    now.n = 0;
    bt_rkey_diff(&old, &now, emit, NULL);
    CT_EQ(s_n, 4);
    CT_EQ(old.n, 0);
}

static void test_ipc_frames(void) {
    uint8_t buf[BT_IPC_HDR + 8], pl[5] = {3, 0x07, 0x00, 0x04, 1};
    size_t n = bt_ipc_frame(buf, BT_IPC_KEY, pl, sizeof pl);
    CT_EQ(n, BT_IPC_HDR + 5);
    uint8_t type;
    const uint8_t *p;
    size_t len;
    CT_EQ(bt_ipc_parse(buf, n - 1, &type, &p, &len), 0); /* incomplete */
    CT_EQ(bt_ipc_parse(buf, n, &type, &p, &len), n);
    CT_EQ(type, BT_IPC_KEY);
    CT_EQ(len, 5);
    CT_MEM_EQ(p, pl, 5);
    uint8_t bad[3] = {BT_IPC_PING, 0xFF, 0xFF};
    CT_EQ(bt_ipc_parse(bad, 3, &type, &p, &len), (size_t)-1);
}

int main(void) {
    CT_RUN(test_keyboard_reports);
    CT_RUN(test_six_key_rollover);
    CT_RUN(test_consumer);
    CT_RUN(test_remote_diff);
    CT_RUN(test_ipc_frames);
    return CT_DONE();
}
