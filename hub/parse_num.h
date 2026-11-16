#ifndef HP_PARSE_NUM_H
#define HP_PARSE_NUM_H

#include <stdlib.h>

/* Strict decimal parse of the whole string into [lo, hi]. 0 on success, -1 on junk/empty/range. */
static inline int hp_parse_num(const char *s, long lo, long hi, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

#endif
