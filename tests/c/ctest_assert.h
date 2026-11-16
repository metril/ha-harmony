#ifndef CTEST_ASSERT_H
#define CTEST_ASSERT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ct_failures;

#define CT_CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); ct_failures++; } } while (0)
#define CT_EQ(a, b) \
    do { long long _a = (long long)(a), _b = (long long)(b); \
         if (_a != _b) { fprintf(stderr, "%s:%d: FAIL: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); ct_failures++; } } while (0)
#define CT_MEM_EQ(a, b, n) CT_CHECK(memcmp((a), (b), (n)) == 0)
#define CT_RUN(fn) do { fn(); } while (0)
#define CT_DONE() (ct_failures ? (fprintf(stderr, "%d failure(s)\n", ct_failures), 1) : 0)

#endif
