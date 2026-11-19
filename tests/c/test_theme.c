/* Theme tables and integer derivation: the generated presets equal theme.h's tokens (default), and
 * ink/pressed/snap/contrast/outline/wire decode match the shared vectors (tests/vectors/themes.json). */
#include "ctest_assert.h"
#include "cJSON.h"
#include "remote/app/theme.h"
#include "remote/app/theme_derive.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static uint32_t U(const cJSON *a, int i) { return (uint32_t)cJSON_GetArrayItem(a, i)->valuedouble; }

static int unhex(const char *s, uint8_t *out, unsigned max)
{
    unsigned n = 0;
    for (; s[0] && s[1] && n < max; s += 2, n++) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n] = (uint8_t)v;
    }
    return (int)n;
}

static void test_default_matches_header(void)
{
    const ra_theme_t *t = ra_theme_find(RA_THEME_DEFAULT);
    CT_CHECK(t != NULL);
    CT_EQ(t->bg, RA_HEX_BG); CT_EQ(t->s1, RA_HEX_S1); CT_EQ(t->s2, RA_HEX_S2); CT_EQ(t->s3, RA_HEX_S3);
    CT_EQ(t->tx, RA_HEX_TX); CT_EQ(t->tx2, RA_HEX_TX2); CT_EQ(t->tx3, RA_HEX_TX3); CT_EQ(t->acc, RA_HEX_ACC);
    CT_EQ(t->onacc, RA_HEX_ONACC); CT_EQ(t->warm, RA_HEX_WARM); CT_EQ(t->onwarm, RA_HEX_ONWARM);
    CT_EQ(t->bad, RA_HEX_BAD); CT_EQ(t->onbad, RA_HEX_ONBAD);
    CT_CHECK(ra_theme_find(RA_THEME_CUSTOM_ID) == NULL);
}

static void test_presets_derived(void)
{
    for (unsigned i = 0; i < RA_THEME_COUNT; i++) {
        const ra_theme_t *t = &ra_themes[i];
        for (unsigned p = 0; p < RA_PALETTE_N; p++) {
            CT_EQ(ra_snap565(t->palette[p].fill), t->palette[p].fill);
            CT_EQ(ra_ink(t->palette[p].fill, t->tx, t->bg), t->palette[p].ink);
            CT_EQ(ra_pressed(t->palette[p].fill, t->palette[p].ink), t->palette[p].pressed);
        }
    }
}

static void test_vectors(const cJSON *root)
{
    const cJSON *a, *v;
    a = cJSON_GetObjectItem(root, "lin");
    CT_EQ(cJSON_GetArraySize(a), 256);
    for (int i = 0; i < 256; i++) CT_EQ(U(a, i), ra_lin[i]);
    a = cJSON_GetObjectItem(root, "snap");
    cJSON_ArrayForEach(v, a) CT_EQ(ra_snap565(U(v, 0)), U(v, 1));
    a = cJSON_GetObjectItem(root, "contrast");
    cJSON_ArrayForEach(v, a) CT_EQ(ra_contrast_x100(U(v, 0), U(v, 1)), U(v, 2));
    a = cJSON_GetObjectItem(root, "derive");
    int n = 0;
    cJSON_ArrayForEach(v, a) {
        uint32_t i = ra_ink(U(v, 0), U(v, 1), U(v, 2));
        CT_EQ(i, U(v, 3));
        CT_EQ(ra_pressed(U(v, 0), i), U(v, 4));
        n++;
    }
    CT_CHECK(n >= 300);
    a = cJSON_GetObjectItem(root, "outline");
    cJSON_ArrayForEach(v, a) CT_EQ(ra_needs_outline(U(v, 0), U(v, 1)), cJSON_IsTrue(cJSON_GetArrayItem(v, 2)));
}

static void test_wire(const cJSON *root)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, "wire")) {
        uint8_t b[RA_WIRE_LEN + 4];
        CT_EQ(unhex(cJSON_GetObjectItem(v, "hex")->valuestring, b, sizeof b), RA_WIRE_LEN);
        ra_theme_t t;
        CT_EQ(ra_theme_decode(b, RA_WIRE_LEN, &t), 0);
        const cJSON *fills = cJSON_GetObjectItem(v, "fills");
        const cJSON *tk = cJSON_GetObjectItem(v, "tokens");
        CT_EQ(t.bg, (uint32_t)cJSON_GetObjectItem(tk, "bg")->valuedouble);
        CT_EQ(t.onbad, (uint32_t)cJSON_GetObjectItem(tk, "onbad")->valuedouble);
        for (int p = 0; p < RA_PALETTE_N; p++) {
            CT_EQ(t.palette[p].fill, U(fills, p));
            CT_EQ(t.palette[p].ink, ra_ink(U(fills, p), t.tx, t.bg));
            CT_EQ(t.palette[p].pressed, ra_pressed(U(fills, p), t.palette[p].ink));
        }
    }
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "wire"), 0);
    uint8_t good[RA_WIRE_LEN];
    unhex(cJSON_GetObjectItem(first, "hex")->valuestring, good, sizeof good);
    ra_theme_t t;
    CT_EQ(ra_theme_decode(good, RA_WIRE_LEN - 1, &t), -1);
    CT_EQ(ra_theme_decode(good, RA_WIRE_LEN + 1, &t), -1);
    CT_EQ(ra_theme_decode(good, 0, &t), -1);
    good[0] = 2;
    CT_EQ(ra_theme_decode(good, RA_WIRE_LEN, &t), -2);
    good[0] = 0;
    CT_EQ(ra_theme_decode(good, RA_WIRE_LEN, &t), -2);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_theme <themes.json>\n"); return 2; }
    char *txt = slurp(argv[1]);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    CT_CHECK(root != NULL);
    CT_RUN(test_default_matches_header);
    CT_RUN(test_presets_derived);
    if (root) {
        test_vectors(root);
        test_wire(root);
    }
    cJSON_Delete(root);
    free(txt);
    return CT_DONE();
}
