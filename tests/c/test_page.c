/* Page decoder, hit-testing and tile text; golden vectors from tests/vectors/pages.json. */
#include <stdio.h>

#include "cJSON.h"
#include "ctest_assert.h"
#include "hub/hp_errors.h"
#include "remote/page/page.h"

#ifndef HP_VECTORS
#error "HP_VECTORS (path to pages.json) must be defined"
#endif

static const char *kind_name[] = {"button", "tile", "label"};

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

static const char *vec_path = HP_VECTORS; /* argv[1] overrides (running on the hub) */

static cJSON *load(void) {
    FILE *f = fopen(vec_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", vec_path); exit(1); }
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) { fprintf(stderr, "bad json\n"); exit(1); }
    return j;
}

static void test_valid_vectors(const cJSON *root) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, "valid")) {
        uint8_t raw[300];
        size_t n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, sizeof raw);
        hp_page_t p;
        CT_EQ(hp_page_decode(raw, n, &p), 0);
        CT_EQ(p.page_id, cJSON_GetObjectItem(v, "page_id")->valueint);
        CT_EQ(p.cols, cJSON_GetObjectItem(v, "cols")->valueint);
        CT_EQ(p.rows, cJSON_GetObjectItem(v, "rows")->valueint);
        const cJSON *ws = cJSON_GetObjectItem(v, "widgets");
        CT_EQ(p.count, cJSON_GetArraySize(ws));
        for (int i = 0; i < cJSON_GetArraySize(ws) && i < p.count; i++) {
            const cJSON *w = cJSON_GetArrayItem(ws, i);
            const hp_widget_t *o = &p.widgets[i];
            CT_CHECK(!strcmp(kind_name[o->kind], cJSON_GetObjectItem(w, "kind")->valuestring));
            CT_EQ(o->id, cJSON_GetObjectItem(w, "id")->valueint);
            CT_EQ(o->x, cJSON_GetObjectItem(w, "x")->valueint);
            CT_EQ(o->y, cJSON_GetObjectItem(w, "y")->valueint);
            CT_EQ(o->w, cJSON_GetObjectItem(w, "w")->valueint);
            CT_EQ(o->h, cJSON_GetObjectItem(w, "h")->valueint);
            CT_CHECK(!strcmp(o->label, cJSON_GetObjectItem(w, "label")->valuestring));
            CT_EQ(o->text[0], 0);
        }
    }
}

static void test_invalid_vectors(const cJSON *root) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, "invalid")) {
        uint8_t raw[300];
        size_t n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, sizeof raw);
        hp_page_t p;
        int want = !strcmp(cJSON_GetObjectItem(v, "error")->valuestring, "toobig") ? -HP_E_TOOBIG : -HP_E_INVAL;
        int got = hp_page_decode(raw, n, &p);
        if (got != want) fprintf(stderr, "vector %s: got %d want %d\n", cJSON_GetObjectItem(v, "name")->valuestring, got, want);
        CT_EQ(got, want);
    }
}

static hp_page_t mixed(void) {
    const uint8_t raw[] = {1, 9, 4, 6, 3, 0, 2, 0x10, 0x21, 4, 'M', 'u', 't', 'e', 1, 3, 0x01, 0x42, 4, 'T', 'e', 'm', 'p',
                           2, 4, 0x05, 0x41, 0};
    hp_page_t p;
    CT_EQ(hp_page_decode(raw, sizeof raw, &p), 0);
    return p;
}

static void test_hit(void) {
    /* 4x6 grid in the 240x288 content area below the 32 px bar: cell 60 x 48 px, rows start at y=32+48r */
    hp_page_t p = mixed();
    const hp_widget_t *w = hp_page_hit(&p, 60, 32); /* first px of col 1, row 0 */
    CT_CHECK(w && w->id == 2);
    w = hp_page_hit(&p, 179, 79);
    CT_CHECK(w && w->id == 2);
    CT_CHECK(hp_page_hit(&p, 59, 32) == NULL);  /* col 0 row 0 empty */
    CT_CHECK(hp_page_hit(&p, 180, 32) == NULL); /* col 3 empty */
    CT_CHECK(hp_page_hit(&p, 100, 0) == NULL);  /* the status bar is never a widget... */
    CT_CHECK(hp_page_hit(&p, 100, 31) == NULL); /* ...down to its last pixel */
    w = hp_page_hit(&p, 0, 79); /* row 0 ends at 32+48-1 */
    CT_CHECK(w == NULL);
    w = hp_page_hit(&p, 0, 80); /* row 1 starts at y=80 */
    CT_CHECK(w && w->id == 3 && w->kind == HP_WIDGET_TILE);
    w = hp_page_hit(&p, 239, 319 - 48 * 3);
    CT_CHECK(w && w->id == 3);
    w = hp_page_hit(&p, 239, 319);
    CT_CHECK(w && w->id == 4 && w->kind == HP_WIDGET_LABEL);
    CT_CHECK(hp_page_hit(&p, -1, 40) == NULL);
    CT_CHECK(hp_page_hit(&p, 240, 40) == NULL);
    CT_CHECK(hp_page_hit(&p, 5, 320) == NULL);
}

static void test_tile_text(void) {
    hp_page_t p = mixed();
    CT_EQ(hp_page_set_tile_text(&p, 3, "21.5C", 5), 0);
    CT_CHECK(!strcmp(p.widgets[1].text, "21.5C"));
    CT_EQ(hp_page_set_tile_text(&p, 3, "", 0), 0);
    CT_EQ(p.widgets[1].text[0], 0);
    CT_EQ(hp_page_set_tile_text(&p, 2, "x", 1), -HP_E_NOENT); /* a button */
    CT_EQ(hp_page_set_tile_text(&p, 99, "x", 1), -HP_E_NOENT);
    CT_EQ(hp_page_set_tile_text(&p, 3, "a\xc3", 2), -HP_E_INVAL); /* cut UTF-8 */
    char big[64];
    memset(big, 'z', sizeof big);
    CT_EQ(hp_page_set_tile_text(&p, 3, big, sizeof big), 0);
    CT_EQ(strlen(p.widgets[1].text), HP_PAGE_MAX_TEXT);
    /* 30 ASCII + one 2-byte char would be 32 bytes: the char is dropped whole */
    memset(big, 'a', 30);
    memcpy(big + 30, "\xc3\xa9", 2);
    CT_EQ(hp_page_set_tile_text(&p, 3, big, 32), 0);
    CT_EQ(strlen(p.widgets[1].text), 30);
}

static void test_out_untouched_on_error(void) {
    hp_page_t p;
    memset(&p, 0x5a, sizeof p);
    const uint8_t bad[] = {2, 0, 4, 6, 0};
    CT_EQ(hp_page_decode(bad, sizeof bad, &p), -HP_E_INVAL);
    CT_EQ(p.page_id, 0x5a);
}

int main(int argc, char **argv) {
    if (argc > 1) vec_path = argv[1];
    cJSON *root = load();
    test_valid_vectors(root);
    test_invalid_vectors(root);
    cJSON_Delete(root);
    CT_RUN(test_hit);
    CT_RUN(test_tile_text);
    CT_RUN(test_out_untouched_on_error);
    return CT_DONE();
}
