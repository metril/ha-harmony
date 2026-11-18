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
    }
}

int main(int argc, char **argv) {
    int shard = argc > 2 ? atoi(argv[1]) : 0, shards = argc > 2 ? atoi(argv[2]) : 1;
    FILE *f = fopen(HP_VECTORS, "rb");
    if (!f) { fprintf(stderr, "cannot open vectors\n"); return 1; }
    static char text[1 << 16];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;
    cJSON *root = cJSON_Parse(text);
    if (!root) return 1;
    static const char *sets[] = {"valid", "valid_v2"};
    for (unsigned s = 0; s < 2; s++) {
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
        }
    }
    cJSON_Delete(root);
    fprintf(stderr, "shard %d/%d: %lu accepted, %lu rejected\n", shard, shards, accepted, rejected);
    return CT_DONE();
}
