/* IR encoder tests: shared vectors (tests/vectors/ir, argv[1] = dir)
 * plus error/boundary cases. */
#include "ctest_assert.h"
#include "cJSON.h"
#include "hub/hp_errors.h"
#include "hub/ir/encode.h"

static const char *const k_files[] = {"nec",  "necext", "samsung32", "rc5",   "rc6",
                                      "sirc12", "sirc15", "sirc20",    "pronto_raw"};

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

static void check_vector(const char *file, int idx, const cJSON *v)
{
    const cJSON *code = cJSON_GetObjectItem(v, "code");
    const cJSON *proto = cJSON_GetObjectItem(code, "proto");
    const cJSON *raw = cJSON_GetObjectItem(code, "raw");
    const cJSON *pr = cJSON_GetObjectItem(code, "pronto");
    hp_ir_code_t c = {0};
    uint32_t rawus[HP_IR_MAX_DUR];
    uint16_t words[HP_IR_MAX_DUR];
    hp_ir_signal_t s;
    if (proto) {
        c.kind = HP_IR_CODE_PROTO;
        CT_EQ(hp_ir_proto_from_name(proto->valuestring, &c.u.p.proto), 0);
        c.u.p.addr = (uint32_t)cJSON_GetObjectItem(code, "addr")->valuedouble;
        c.u.p.cmd = (uint32_t)cJSON_GetObjectItem(code, "cmd")->valuedouble;
    } else if (raw) {
        const cJSON *us = cJSON_GetObjectItem(raw, "us"), *e;
        size_t n = 0;
        cJSON_ArrayForEach(e, us) rawus[n++] = (uint32_t)e->valuedouble;
        c.kind = HP_IR_CODE_RAW;
        c.u.raw.hz = (uint32_t)cJSON_GetObjectItem(raw, "hz")->valuedouble;
        c.u.raw.us = rawus;
        c.u.raw.n = n;
    } else {
        size_t n = 0;
        CT_EQ(hp_ir_parse_pronto(pr->valuestring, words, HP_IR_MAX_DUR, &n), 0);
        c.kind = HP_IR_CODE_PRONTO;
        c.u.pronto.words = words;
        c.u.pronto.n = n;
    }
    int toggle = (int)cJSON_GetObjectItem(v, "toggle")->valuedouble;
    int rc = hp_ir_encode(&c, (uint8_t)toggle, &s);
    if (rc != 0) fprintf(stderr, "%s[%d]: encode rc=%d\n", file, idx, rc);
    CT_EQ(rc, 0);
    if (rc) return;
    const cJSON *once = cJSON_GetObjectItem(v, "once"), *rep = cJSON_GetObjectItem(v, "rep"), *e;
    CT_EQ(s.carrier_hz, cJSON_GetObjectItem(v, "carrier_hz")->valuedouble);
    CT_EQ(s.period_us, cJSON_GetObjectItem(v, "period_us")->valuedouble);
    CT_EQ(s.once_n, cJSON_GetArraySize(once));
    CT_EQ(s.rep_n, cJSON_GetArraySize(rep));
    int i = 0, bad = 0;
    cJSON_ArrayForEach(e, once) bad += s.us[i++] != (uint16_t)e->valuedouble;
    cJSON_ArrayForEach(e, rep) bad += s.us[i++] != (uint16_t)e->valuedouble;
    if (bad) fprintf(stderr, "%s[%d]: %d duration mismatches\n", file, idx, bad);
    CT_EQ(bad, 0);
}

static void test_vectors(const char *dir)
{
    for (size_t f = 0; f < sizeof k_files / sizeof *k_files; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.json", dir, k_files[f]);
        char *txt = slurp(path);
        if (!txt) { fprintf(stderr, "cannot read %s\n", path); ct_failures++; continue; }
        cJSON *arr = cJSON_Parse(txt);
        CT_CHECK(arr && cJSON_GetArraySize(arr) > 0);
        int i = 0;
        for (const cJSON *v = arr ? arr->child : NULL; v; v = v->next) check_vector(k_files[f], i++, v);
        cJSON_Delete(arr);
        free(txt);
    }
}

static hp_ir_code_t pcode(hp_ir_proto_t p, uint32_t a, uint32_t m)
{
    hp_ir_code_t c = {.kind = HP_IR_CODE_PROTO};
    c.u.p.proto = p;
    c.u.p.addr = a;
    c.u.p.cmd = m;
    return c;
}

static void test_range_errors(void)
{
    static const struct { hp_ir_proto_t p; uint32_t amax, cmax; } r[] = {
        {HP_IR_NEC, 0xFF, 0xFF},     {HP_IR_NECEXT, 0xFFFF, 0xFFFF}, {HP_IR_SAMSUNG32, 0xFF, 0xFF},
        {HP_IR_RC5, 0x1F, 0x7F},     {HP_IR_RC6, 0xFF, 0xFF},      {HP_IR_SIRC12, 0x1F, 0x7F},
        {HP_IR_SIRC15, 0xFF, 0x7F},  {HP_IR_SIRC20, 0x1FFF, 0x7F},
    };
    hp_ir_signal_t s;
    for (size_t i = 0; i < sizeof r / sizeof *r; i++) {
        hp_ir_code_t ok = pcode(r[i].p, r[i].amax, r[i].cmax);
        hp_ir_code_t ba = pcode(r[i].p, r[i].amax + 1, 0), bc = pcode(r[i].p, 0, r[i].cmax + 1);
        CT_EQ(hp_ir_encode(&ok, 0, &s), 0);
        CT_CHECK(s.once_n > 0 && s.once_n + s.rep_n <= HP_IR_MAX_DUR);
        CT_EQ(hp_ir_encode(&ba, 0, &s), -HP_E_INVAL);
        CT_EQ(s.once_n + s.rep_n, 0);
        CT_EQ(hp_ir_encode(&bc, 0, &s), -HP_E_INVAL);
    }
    hp_ir_code_t bad = pcode((hp_ir_proto_t)99, 0, 0);
    CT_EQ(hp_ir_encode(&bad, 0, &s), -HP_E_INVAL);
    CT_EQ(hp_ir_encode(NULL, 0, &s), -HP_E_INVAL);
}

static int pronto(const char *txt, hp_ir_signal_t *s)
{
    uint16_t w[64];
    size_t n = 0;
    int rc = hp_ir_parse_pronto(txt, w, 64, &n);
    if (rc) return rc;
    hp_ir_code_t c = {.kind = HP_IR_CODE_PRONTO};
    c.u.pronto.words = w;
    c.u.pronto.n = n;
    return hp_ir_encode(&c, 0, s);
}

static void test_pronto_errors(void)
{
    hp_ir_signal_t s;
    CT_EQ(pronto("0000 006D 0001 0000 0010 0010", &s), 0);
    CT_EQ(pronto("0100 006D 0001 0000 0010 0010", &s), -HP_E_INVAL);      /* format word */
    CT_EQ(pronto("0000 0000 0001 0000 0010 0010", &s), -HP_E_INVAL);      /* freq 0 */
    CT_EQ(pronto("0000 006D 0000 0000", &s), -HP_E_INVAL);                /* no bursts */
    CT_EQ(pronto("0000 006D 0001 0000 0010", &s), -HP_E_INVAL);           /* odd list */
    CT_EQ(pronto("0000 006D 0002 0000 0010 0010", &s), -HP_E_INVAL);      /* count mismatch */
    CT_EQ(pronto("0000 006D 0001 0000 0010 0010 0010 0010", &s), -HP_E_INVAL);
    CT_EQ(pronto("0000 006D 0001 0000 0000 0010", &s), -HP_E_INVAL);      /* zero burst */
    CT_EQ(pronto("0000 006D 0001", &s), -HP_E_INVAL);                     /* short header */
    CT_EQ(pronto("", &s), -HP_E_INVAL);
    CT_EQ(pronto("0000 006G 0001 0000 0010 0010", &s), -HP_E_INVAL);      /* bad hex */
    CT_EQ(pronto("00000 006D", &s), -HP_E_INVAL);                         /* >4 digits */
    CT_EQ(pronto("0000 006D 0001 0000 FFFF 0010", &s), -HP_E_TOOBIG);     /* >65535 us */
    CT_EQ(s.once_n, 0);
    CT_EQ(pronto("0000 006D 0001 0000 0010 0010 0010", &s), -HP_E_INVAL);

    uint16_t w[4];
    size_t n = 0;
    CT_EQ(hp_ir_parse_pronto("1 2 3 4 5", w, 4, &n), -HP_E_TOOBIG);
    CT_EQ(hp_ir_parse_pronto("1 2 3 4", w, 4, &n), 0);
    CT_EQ(n, 4);
    CT_EQ(w[3], 4);
    CT_EQ(hp_ir_parse_pronto("  0A\t0b  ", w, 4, &n), 0);
    CT_EQ(w[0] == 0x0A && w[1] == 0x0B, 1);

    /* too many durations: 257 pairs = 514 > HP_IR_MAX_DUR */
    static uint16_t big[4 + 514];
    big[1] = 0x6D;
    big[2] = 257;
    for (int i = 0; i < 514; i++) big[4 + i] = 1;
    hp_ir_code_t c = {.kind = HP_IR_CODE_PRONTO};
    c.u.pronto.words = big;
    c.u.pronto.n = sizeof big / sizeof *big;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_TOOBIG);
    big[2] = 256; /* exactly the limit: 512 durations */
    c.u.pronto.n = 4 + 512;
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    CT_EQ(s.once_n, 512);
}

static void test_raw(void)
{
    hp_ir_signal_t s;
    uint32_t ok[] = {100, 200, 300, 65535};
    uint32_t odd[] = {100, 200, 300};
    uint32_t zero[] = {100, 0};
    uint32_t big[] = {100, 65536};
    static uint32_t many[HP_IR_MAX_DUR + 2];
    hp_ir_code_t c = {.kind = HP_IR_CODE_RAW};
    c.u.raw.hz = 38000;
    c.u.raw.us = ok;
    c.u.raw.n = 4;
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    CT_EQ(s.rep_n, 0);
    CT_EQ(s.us[3], 65535);
    c.u.raw.us = odd; c.u.raw.n = 3;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_INVAL);
    c.u.raw.us = zero; c.u.raw.n = 2;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_INVAL);
    c.u.raw.us = big; c.u.raw.n = 2;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_TOOBIG);
    for (size_t i = 0; i < HP_IR_MAX_DUR + 2; i++) many[i] = 10;
    c.u.raw.us = many; c.u.raw.n = HP_IR_MAX_DUR;
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    c.u.raw.n = HP_IR_MAX_DUR + 2;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_TOOBIG);
    c.u.raw.us = ok; c.u.raw.n = 4; c.u.raw.hz = 0;
    CT_EQ(hp_ir_encode(&c, 0, &s), -HP_E_INVAL);
}

static void test_names(void)
{
    static const char *const n[] = {"nec", "necext", "samsung32", "rc5", "rc6", "sirc12", "sirc15", "sirc20"};
    hp_ir_proto_t p;
    for (int i = 0; i < 8; i++) {
        CT_EQ(hp_ir_proto_from_name(n[i], &p), 0);
        CT_EQ(p, i);
        CT_CHECK(strcmp(hp_ir_proto_name(p), n[i]) == 0);
    }
    CT_EQ(hp_ir_proto_from_name("sirc", &p), -HP_E_NOENT);
    CT_EQ(hp_ir_proto_from_name("NEC", &p), -HP_E_NOENT);
    CT_EQ(hp_ir_proto_from_name(NULL, &p), -HP_E_NOENT);
    CT_CHECK(hp_ir_proto_name((hp_ir_proto_t)42) == NULL);
}

static void test_periods(void)
{
    /* NEC: gap clamped to 65535, nominal period reported; SIRC fits exactly */
    hp_ir_signal_t s;
    hp_ir_code_t c = pcode(HP_IR_NEC, 4, 8);
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    CT_EQ(s.period_us, 108000);
    CT_EQ(s.us[s.once_n - 1], 39970);
    CT_EQ(s.us[s.once_n + s.rep_n - 1], 65535);
    CT_EQ(s.rep_n, 4);
    CT_EQ(s.us[s.once_n], 9000);
    CT_EQ(s.us[s.once_n + 1], 2250);
    c = pcode(HP_IR_SIRC12, 1, 21);
    CT_EQ(hp_ir_encode(&c, 0, &s), 0);
    uint32_t a = 0;
    for (int i = 0; i < s.once_n; i++) a += s.us[i];
    CT_EQ(a, 45000);
    CT_EQ(s.period_us, 45000);
    CT_EQ(s.once_n, s.rep_n);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <vector-dir>\n", argv[0]); return 2; }
    CT_RUN(test_names);
    CT_RUN(test_range_errors);
    CT_RUN(test_pronto_errors);
    CT_RUN(test_raw);
    CT_RUN(test_periods);
    test_vectors(argv[1]);
    return CT_DONE();
}
