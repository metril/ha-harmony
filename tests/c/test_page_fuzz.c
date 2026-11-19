/* Page decoder robustness: every single-byte mutation (all 255 other values) and every truncation of each valid
 * vector (v1 and v2) decodes without a crash or over-read (the input is an exact-size heap block, so ASan flags
 * one byte past it), and whatever is accepted obeys the decoder's own invariants.
 * argv: shard index and count (the mutations of a vector are split by byte position, so ctest runs them in parallel). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "remote/page/page.h"

#ifndef HP_VECTORS
#error "HP_VECTORS must be defined"
#endif

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s) / 2;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        sscanf(s + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
    return n;
}

static unsigned long accepted, rejected;

static void try_decode(const uint8_t *src, size_t n) {
    uint8_t *buf = malloc(n ? n : 1); /* exact size: an over-read is an ASan error */
    memcpy(buf, src, n);
    hp_page_t p;
    memset(&p, 0xA5, sizeof p);
    int rc = hp_page_decode(buf, n, &p);
    free(buf);
    if (rc < 0) {
        rejected++;
        CT_CHECK(rc == -HP_E_INVAL || rc == -HP_E_TOOBIG);
        CT_EQ(p.page_id, 0xA5); /* *out untouched on error */
        return;
    }
    accepted++;
    CT_CHECK(p.version == 1 || p.version == 2);
    CT_CHECK(p.count <= HP_PAGE_MAX_WIDGETS);
    CT_CHECK(strlen(p.title) <= HP_PAGE_MAX_TITLE);
    for (unsigned i = 0; i < p.count; i++) {
        const hp_widget_t *w = &p.widgets[i];
        CT_CHECK(w->id <= 127 && w->kind <= HP_WIDGET_LABEL && w->role <= HP_ROLE_NAV && w->icon != 255);
        CT_CHECK(w->w && w->h && w->x + w->w <= p.cols && w->y + w->h <= p.rows);
        CT_CHECK(strlen(w->label) <= HP_PAGE_MAX_LABEL);
        CT_CHECK(w->role != HP_ROLE_NAV || (w->has_bind && w->bind < p.total));
        CT_CHECK(w->color_fill.kind <= HP_COLOR_RGB && w->color_on.kind <= HP_COLOR_RGB);
        CT_CHECK(w->color_fill.kind != HP_COLOR_PALETTE || (w->color_fill.value >= 1 && w->color_fill.value <= HP_PAGE_PALETTE_MAX));
        CT_CHECK(w->color_on.kind != HP_COLOR_PALETTE || (w->color_on.value >= 1 && w->color_on.value <= HP_PAGE_PALETTE_MAX));
        CT_CHECK(w->color_on.kind == HP_COLOR_NONE || w->kind == HP_WIDGET_TILE); /* on colour is tiles only */
        CT_CHECK(p.version == 2 || (w->color_fill.kind == HP_COLOR_NONE && w->color_on.kind == HP_COLOR_NONE));
    }
    CT_CHECK(p.theme <= 7 && (p.version == 2 || p.theme == 0));
}

static uint64_t xs(uint64_t *s) { /* xorshift64* */
    *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
    return *s * 0x2545F4914F6CDD1Dull;
}

int main(int argc, char **argv) {
    const char *ie = getenv("HP_FUZZ_ITERS");
    unsigned iters = ie ? (unsigned)strtoul(ie, NULL, 10) : 20000;
    int shard = argc > 2 ? atoi(argv[1]) : 0, shards = argc > 2 ? atoi(argv[2]) : 1;
    FILE *f = fopen(HP_VECTORS, "rb");
    if (!f) { fprintf(stderr, "cannot open vectors\n"); return 1; }
    static char text[1 << 18];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;
    cJSON *root = cJSON_Parse(text);
    if (!root) return 1;
    static const char *sets[] = {"valid", "valid_v2", "valid_v2c"};
    for (unsigned s = 0; s < 3; s++) {
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, sets[s])) {
            uint8_t raw[300];
            size_t n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, sizeof raw);
            if (shard == 0)
                for (size_t k = 0; k <= n; k++) try_decode(raw, k); /* every truncation (0 .. n) */
            for (size_t i = (size_t)shard; i < n; i += (size_t)shards) {
                uint8_t m[300];
                memcpy(m, raw, n);
                for (unsigned b = 0; b < 256; b++) {
                    if (b == raw[i]) continue;
                    m[i] = (uint8_t)b;
                    try_decode(m, n);
                }
            }
            /* seeded random phase: 1..4 random byte overwrites, an insertion, a deletion (HP_FUZZ_ITERS per vector per shard) */
            uint64_t rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)shard << 32) ^ n;
            for (unsigned it = 0; it < iters; it++) {
                uint8_t m[300];
                size_t mn = n;
                memcpy(m, raw, n);
                unsigned op = (unsigned)(xs(&rng) % 6);
                if (op < 4) {
                    for (unsigned k = 0; k <= op && n; k++) m[xs(&rng) % n] = (uint8_t)xs(&rng);
                } else if (op == 4 && n < sizeof m) {
                    size_t at = xs(&rng) % (n + 1);
                    memmove(m + at + 1, m + at, n - at);
                    m[at] = (uint8_t)xs(&rng);
                    mn = n + 1;
                } else if (n) {
                    size_t at = xs(&rng) % n;
                    memmove(m + at, m + at + 1, n - at - 1);
                    mn = n - 1;
                }
                try_decode(m, mn);
            }
        }
    }
    cJSON_Delete(root);
    fprintf(stderr, "shard %d/%d: %lu accepted, %lu rejected\n", shard, shards, accepted, rejected);
    return CT_DONE();
}
