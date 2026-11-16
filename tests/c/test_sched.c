#include "ctest_assert.h"
#include "hub/engine/sched.h"

static uint32_t g_now;
static uint32_t clk(void *c) { (void)c; return g_now; }

static char order[64];
static int norder;
static void mark(void *arg, uint32_t now) { (void)now; order[norder++] = *(const char *)arg; order[norder] = 0; }

static void test_order_and_timeout(void) {
    hp_sched_t s;
    hp_sched_init(&s, clk, NULL);
    g_now = 1000;
    norder = 0;
    CT_EQ(hp_sched_next_timeout_ms(&s), -1);
    hp_sched_after(&s, 30, mark, "c");
    hp_sched_after(&s, 10, mark, "a");
    hp_sched_after(&s, 10, mark, "b");
    CT_EQ(hp_sched_next_timeout_ms(&s), 10);
    g_now = 1004;
    CT_EQ(hp_sched_next_timeout_ms(&s), 6);
    CT_EQ(hp_sched_run(&s, g_now), 0);
    g_now = 1010;
    CT_EQ(hp_sched_next_timeout_ms(&s), 0);
    CT_EQ(hp_sched_run(&s, g_now), 2);
    CT_CHECK(!strcmp(order, "ab")); /* same due: scheduling order */
    g_now = 1500;
    CT_EQ(hp_sched_run(&s, g_now), 1);
    CT_CHECK(!strcmp(order, "abc"));
    CT_EQ(hp_sched_next_timeout_ms(&s), -1);
}

static void test_cancel(void) {
    hp_sched_t s;
    hp_sched_init(&s, clk, NULL);
    g_now = 0;
    norder = 0;
    hp_sched_handle a = hp_sched_after(&s, 5, mark, "a"), b = hp_sched_after(&s, 5, mark, "b");
    CT_CHECK(a && b && a != b);
    CT_EQ(hp_sched_cancel(&s, a), 1);
    CT_EQ(hp_sched_cancel(&s, a), 0);
    CT_EQ(hp_sched_cancel(&s, 0), 0);
    CT_EQ(hp_sched_cancel(&s, 0xFFFFFFFF), 0);
    hp_sched_after(&s, 5, mark, "c"); /* reuses a's slot: the old handle stays dead */
    CT_EQ(hp_sched_cancel(&s, a), 0);
    g_now = 5;
    CT_EQ(hp_sched_run(&s, g_now), 2);
    CT_CHECK(!strcmp(order, "bc"));
    CT_EQ(hp_sched_cancel(&s, b), 0); /* already ran */
}

static void test_capacity(void) {
    hp_sched_t s;
    hp_sched_init(&s, clk, NULL);
    g_now = 0;
    hp_sched_handle h[HP_SCHED_CAP];
    for (int i = 0; i < HP_SCHED_CAP; i++) CT_CHECK((h[i] = hp_sched_after(&s, 1, mark, "x")) != 0);
    CT_EQ(hp_sched_after(&s, 1, mark, "x"), 0);
    CT_EQ(hp_sched_cancel(&s, h[3]), 1);
    CT_CHECK(hp_sched_after(&s, 1, mark, "x") != 0);
    CT_EQ(hp_sched_after(&s, 1, NULL, NULL), 0);
}

typedef struct { hp_sched_t *s; int n; uint32_t at[8]; } resched_t;
static void again(void *arg, uint32_t now) {
    resched_t *r = arg;
    r->at[r->n++] = now;
    if (r->n < 4) CT_CHECK(hp_sched_after(r->s, r->n == 1 ? 0 : 100, again, r) != 0);
}

static void test_reschedule(void) {
    hp_sched_t s;
    hp_sched_init(&s, clk, NULL);
    g_now = 50;
    resched_t r = {&s, 0, {0}};
    hp_sched_after(&s, 0, again, &r);
    CT_EQ(hp_sched_run(&s, g_now), 1); /* 0-delay reschedule does not run in the same pass */
    CT_EQ(r.n, 1);
    CT_EQ(hp_sched_run(&s, g_now), 1);
    CT_EQ(r.n, 2);
    CT_EQ(hp_sched_next_timeout_ms(&s), 100);
    g_now = 150;
    hp_sched_run(&s, g_now);
    g_now = 250;
    hp_sched_run(&s, g_now);
    CT_EQ(r.n, 4);
    CT_EQ(r.at[3], 250);
    CT_EQ(hp_sched_pending(&s), 0);
}

static void test_wraparound(void) {
    hp_sched_t s;
    hp_sched_init(&s, clk, NULL);
    g_now = 0xFFFFFFF0u;
    norder = 0;
    hp_sched_after(&s, 0x20, mark, "w");
    CT_EQ(hp_sched_next_timeout_ms(&s), 0x20);
    g_now = 0x0000000Fu;
    CT_EQ(hp_sched_run(&s, g_now), 0);
    g_now = 0x00000010u;
    CT_EQ(hp_sched_run(&s, g_now), 1);
}

int main(void) {
    CT_RUN(test_order_and_timeout);
    CT_RUN(test_cancel);
    CT_RUN(test_capacity);
    CT_RUN(test_reschedule);
    CT_RUN(test_wraparound);
    return CT_DONE();
}
