#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "hub/rf/framing.h"

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + 3);
}

static hp_rf_msg_t msg;
static hp_rf_frame_t fr[16];

static void roundtrip(size_t len, size_t cap, size_t want_frags)
{
    uint8_t p[HP_RF_MSG_MAX];
    size_t n = 0;
    hp_rf_asm_t a;
    fill(p, len);
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 9, p, len, cap, fr, 16, &n), 0);
    CT_EQ(n, want_frags);
    hp_rf_asm_init(&a);
    for (size_t i = 0; i < n; i++) {
        CT_CHECK(fr[i].len <= cap);
        int r = hp_rf_asm_push(&a, fr[i].data, fr[i].len, &msg);
        CT_EQ(r, i + 1 == n ? HP_ASM_COMPLETE : HP_ASM_INCOMPLETE);
    }
    CT_EQ(msg.len, len);
    CT_EQ(msg.seq, 9);
    CT_EQ(msg.type, HP_MSG_BATTERY);
    CT_MEM_EQ(msg.payload, p, len);
}

static void test_roundtrips(void)
{
    roundtrip(0, 30, 1);
    roundtrip(5, 30, 1);
    roundtrip(26, 30, 1);
    roundtrip(27, 30, 2);
    roundtrip(28, 32, 1);
    roundtrip(29, 32, 2);
    roundtrip(100, 30, 4);
    roundtrip(100, 32, 4);
    roundtrip(15 * 26, 30, 15);
    roundtrip(15 * 28, 32, 15);
}

static void test_toobig(void)
{
    uint8_t p[500] = {0};
    size_t n = 99;
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, p, 15 * 26 + 1, 30, fr, 16, &n), -HP_E_TOOBIG);
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, p, 15 * 28 + 1, 32, fr, 16, &n), -HP_E_TOOBIG);
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, p, 100, 30, fr, 3, &n), -HP_E_TOOBIG);
    CT_EQ(n, 99); /* untouched on failure */
}

static void test_invalid_args(void)
{
    uint8_t p[4] = {0};
    size_t n;
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, p, 4, 4, fr, 16, &n), -HP_E_INVAL);
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, p, 4, 33, fr, 16, &n), -HP_E_INVAL);
    CT_EQ(hp_rf_split(HP_MSG_BATTERY, 1, NULL, 4, 30, fr, 16, &n), -HP_E_INVAL);
}

static void test_duplicate_and_order(void)
{
    uint8_t p[80];
    size_t n;
    hp_rf_asm_t a;
    fill(p, 80);
    CT_EQ(hp_rf_split(HP_MSG_KEY_EVENT, 3, p, 80, 30, fr, 16, &n), 0);
    CT_EQ(n, 4);
    hp_rf_asm_init(&a);
    CT_EQ(hp_rf_asm_push(&a, fr[2].data, fr[2].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[2].data, fr[2].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[0].data, fr[0].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[0].data, fr[0].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[3].data, fr[3].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[1].data, fr[1].len, &msg), HP_ASM_COMPLETE);
    CT_EQ(msg.len, 80);
    CT_MEM_EQ(msg.payload, p, 80);
}

static void test_mismatched_total(void)
{
    uint8_t p[80];
    size_t n;
    hp_rf_asm_t a;
    fill(p, 80);
    CT_EQ(hp_rf_split(HP_MSG_KEY_EVENT, 3, p, 80, 30, fr, 16, &n), 0);
    hp_rf_asm_init(&a);
    CT_EQ(hp_rf_asm_push(&a, fr[0].data, fr[0].len, &msg), HP_ASM_INCOMPLETE);
    fr[1].data[2] = (uint8_t)((1 << 4) | 5); /* total 5 != 4 */
    CT_EQ(hp_rf_asm_push(&a, fr[1].data, fr[1].len, &msg), -HP_E_INVAL);
    fr[1].data[2] = (uint8_t)((1 << 4) | 4); /* state intact: still completes */
    CT_EQ(hp_rf_asm_push(&a, fr[1].data, fr[1].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[2].data, fr[2].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[3].data, fr[3].len, &msg), HP_ASM_COMPLETE);
    CT_MEM_EQ(msg.payload, p, 80);
}

static void test_new_seq_discards_partial(void)
{
    uint8_t p[80], q[10];
    size_t n, m;
    hp_rf_frame_t g[2];
    hp_rf_asm_t a;
    fill(p, 80);
    memset(q, 0xAB, sizeof q);
    CT_EQ(hp_rf_split(HP_MSG_KEY_EVENT, 3, p, 80, 30, fr, 16, &n), 0);
    CT_EQ(hp_rf_split(HP_MSG_KEY_EVENT, 4, q, 10, 30, g, 2, &m), 0);
    hp_rf_asm_init(&a);
    CT_EQ(hp_rf_asm_push(&a, fr[0].data, fr[0].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[1].data, fr[1].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, g[0].data, g[0].len, &msg), HP_ASM_COMPLETE);
    CT_EQ(msg.seq, 4);
    CT_EQ(msg.len, 10);
    CT_MEM_EQ(msg.payload, q, 10);
    /* old stream's remaining fragments must not complete a message */
    CT_EQ(hp_rf_asm_push(&a, fr[2].data, fr[2].len, &msg), HP_ASM_INCOMPLETE);
    CT_EQ(hp_rf_asm_push(&a, fr[3].data, fr[3].len, &msg), HP_ASM_INCOMPLETE);
}

static void test_malformed(void)
{
    hp_rf_asm_t a;
    uint8_t f[HP_RF_FRAME_MAX] = {1, 1, (0 << 4) | 1, 0};
    hp_rf_asm_init(&a);
    for (size_t l = 0; l < HP_RF_HDR_LEN; l++)
        CT_EQ(hp_rf_asm_push(&a, f, l, &msg), -HP_E_INVAL);
    f[2] = 0x00; /* total 0 */
    CT_EQ(hp_rf_asm_push(&a, f, 4, &msg), -HP_E_INVAL);
    f[2] = 0x20 | 2; /* index == total... index 2 total 2 */
    CT_EQ(hp_rf_asm_push(&a, f, 4, &msg), -HP_E_INVAL);
    f[2] = 0xf0 | 3; /* index > total */
    CT_EQ(hp_rf_asm_push(&a, f, 4, &msg), -HP_E_INVAL);
    f[2] = 0x01;
    f[3] = 10; /* len larger than frame */
    CT_EQ(hp_rf_asm_push(&a, f, 8, &msg), -HP_E_INVAL);
    f[3] = 33; /* len larger than any slot */
    CT_EQ(hp_rf_asm_push(&a, f, 32, &msg), -HP_E_INVAL);
    f[3] = 255;
    CT_EQ(hp_rf_asm_push(&a, f, 32, &msg), -HP_E_INVAL);
    f[3] = 28; /* valid boundary after errors */
    CT_EQ(hp_rf_asm_push(&a, f, 32, &msg), HP_ASM_COMPLETE);
    CT_EQ(msg.len, 28);
}

int main(void)
{
    CT_RUN(test_roundtrips);
    CT_RUN(test_toobig);
    CT_RUN(test_invalid_args);
    CT_RUN(test_duplicate_and_order);
    CT_RUN(test_mismatched_total);
    CT_RUN(test_new_seq_discards_partial);
    CT_RUN(test_malformed);
    return CT_DONE();
}
