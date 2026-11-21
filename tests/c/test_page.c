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
    static char buf[1 << 18];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) { fprintf(stderr, "bad json\n"); exit(1); }
    return j;
}

static const char *kind_str[] = {"normal", "home", "favorites"};
static const char *role_str[] = {"default", "primary", "secondary", "danger", "nav"};
static const char *align_str[] = {"center", "left", "right", "?"};
static const char *shape_str[] = {"card", "row", "pill", "monogram"};

static const char *palette_str[] = {"red", "orange", "amber", "yellow", "lime", "green", "teal", "cyan", "blue", "indigo", "purple", "pink"};

/* A colour equals the dict's null | palette name | "#RRGGBB" (RGB565 expanded: R,B = v<<3, G = v<<2). */
static void check_color(const hp_color_t *c, const cJSON *j) {
    if (cJSON_IsNull(j)) {
        CT_EQ(c->kind, HP_COLOR_NONE);
        CT_EQ(c->value, 0);
    } else if (c->kind == HP_COLOR_PALETTE) {
        CT_CHECK(c->value >= 1 && c->value <= HP_PAGE_PALETTE_MAX);
        CT_CHECK(!strcmp(palette_str[c->value - 1], j->valuestring));
    } else {
        char hx[8];
        unsigned v = c->value;
        snprintf(hx, sizeof hx, "#%02X%02X%02X", ((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3);
        CT_EQ(c->kind, HP_COLOR_RGB);
        CT_CHECK(!strcmp(hx, j->valuestring));
    }
}

static int jint(const cJSON *o, const char *k) { return cJSON_GetObjectItem(o, k)->valueint; }
static const char *jstr(const cJSON *o, const char *k) { return cJSON_GetObjectItem(o, k)->valuestring; }

/* One decoded page must equal the vector's `dict` model (the contract the TypeScript renderer mirrors) field for field. */
static void check_dict(const char *name, const hp_page_t *p, const cJSON *m) {
    int bad = ct_failures;
    CT_EQ(p->version, jint(m, "version"));
    CT_EQ(p->page_id, jint(m, "id"));
    CT_EQ(p->cols, jint(m, "cols"));
    CT_EQ(p->rows, jint(m, "rows"));
    CT_EQ(p->gen, jint(m, "gen"));
    CT_EQ(p->total, jint(m, "total"));
    CT_CHECK(!strcmp(kind_str[p->kind], jstr(m, "kind")));
    CT_EQ(p->back, cJSON_IsTrue(cJSON_GetObjectItem(m, "back")));
    const cJSON *sq = cJSON_GetObjectItem(m, "seq");
    if (cJSON_IsNull(sq)) {
        CT_EQ(p->seq_cnt, 0);
        CT_EQ(p->seq_idx, 0);
    } else {
        CT_EQ(p->seq_idx, jint(sq, "idx"));
        CT_EQ(p->seq_cnt, jint(sq, "cnt"));
    }
    CT_EQ(p->act, jint(m, "act"));
    CT_EQ(p->theme, jint(m, "theme"));
    CT_CHECK(!strcmp(p->title, jstr(m, "title")));
    const cJSON *ws = cJSON_GetObjectItem(m, "widgets");
    CT_EQ(p->count, cJSON_GetArraySize(ws));
    for (int i = 0; i < cJSON_GetArraySize(ws) && i < p->count; i++) {
        const cJSON *w = cJSON_GetArrayItem(ws, i);
        const hp_widget_t *o = &p->widgets[i];
        CT_CHECK(!strcmp(kind_name[o->kind], jstr(w, "kind")));
        CT_EQ(o->id, jint(w, "id"));
        CT_EQ(o->x, jint(w, "x"));
        CT_EQ(o->y, jint(w, "y"));
        CT_EQ(o->w, jint(w, "w"));
        CT_EQ(o->h, jint(w, "h"));
        CT_CHECK(!strcmp(o->label, jstr(w, "label")));
        CT_CHECK(!strcmp(role_str[o->role], jstr(w, "role")));
        CT_CHECK(!strcmp(shape_str[o->shape], jstr(w, "shape")));
        CT_EQ(o->icon_only, cJSON_IsTrue(cJSON_GetObjectItem(w, "icon_only")));
        CT_EQ(o->icon, jint(w, "icon"));
        CT_EQ(o->trailing, cJSON_IsTrue(cJSON_GetObjectItem(w, "trailing")));
        {
            const cJSON *al = cJSON_GetObjectItem(w, "align");
            CT_CHECK(!strcmp(align_str[o->align], al ? al->valuestring : "center"));
        }
        const cJSON *bd = cJSON_GetObjectItem(w, "bind");
        CT_EQ(o->has_bind, !cJSON_IsNull(bd));
        if (o->has_bind) CT_EQ(o->bind, bd->valueint);
        check_color(&o->color_fill, cJSON_GetObjectItem(w, "color"));
        check_color(&o->color_on, cJSON_GetObjectItem(w, "on_color"));
        CT_EQ(o->text[0], 0);
        CT_EQ(o->tstate, 0);
    }
    if (ct_failures != bad) fprintf(stderr, "vector %s: model mismatch\n", name);
}

static void test_valid_vectors(const cJSON *root, const char *set) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, set)) {
        uint8_t raw[300];
        size_t n = unhex(jstr(v, "hex"), raw, sizeof raw);
        hp_page_t p;
        CT_EQ(hp_page_decode(raw, n, &p), 0);
        check_dict(jstr(v, "name"), &p, cJSON_GetObjectItem(v, "dict"));
    }
}

static void test_invalid_vectors(const cJSON *root, const char *set) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, set)) {
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
    test_valid_vectors(root, "valid");
    test_invalid_vectors(root, "invalid");
    test_valid_vectors(root, "valid_v2");
    test_valid_vectors(root, "valid_v2c");
    test_invalid_vectors(root, "invalid_v2");
    cJSON_Delete(root);
    CT_RUN(test_hit);
    CT_RUN(test_tile_text);
    CT_RUN(test_out_untouched_on_error);
    return CT_DONE();
}
