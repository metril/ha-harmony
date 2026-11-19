#include "remote/app/theme_derive.h"

#define K 2047500u

static uint32_t lum(uint32_t c)
{
    return 2126u * ra_lin[(c >> 16) & 255] + 7152u * ra_lin[(c >> 8) & 255] + 722u * ra_lin[c & 255];
}

static void hilo(uint32_t a, uint32_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t ya = (uint64_t)lum(a) + K, yb = (uint64_t)lum(b) + K;
    *hi = ya >= yb ? ya : yb;
    *lo = ya >= yb ? yb : ya;
}

uint32_t ra_snap565(uint32_t c)
{
    uint32_t r = (((c >> 16) & 255) + 4) >> 3, g = (((c >> 8) & 255) + 2) >> 2, b = ((c & 255) + 4) >> 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (r << 19) | (g << 10) | (b << 3);
}

uint32_t ra_contrast_x100(uint32_t a, uint32_t b)
{
    uint64_t hi, lo;
    hilo(a, b, &hi, &lo);
    return (uint32_t)(100 * hi / lo);
}

int ra_meets(uint32_t a, uint32_t b, uint32_t num, uint32_t den)
{
    uint64_t hi, lo;
    hilo(a, b, &hi, &lo);
    return den * hi >= num * lo;
}

uint32_t ra_ink(uint32_t fill, uint32_t tx, uint32_t bg)
{
    uint64_t nt, dt, nb, db;
    hilo(fill, tx, &nt, &dt);
    hilo(fill, bg, &nb, &db);
    return nt * db >= nb * dt ? tx : bg;
}

uint32_t ra_pressed(uint32_t fill, uint32_t ink)
{
    uint32_t c = 0;
    for (int s = 16; s >= 0; s -= 8)
        c |= ((3 * ((fill >> s) & 255) + ((ink >> s) & 255) + 2) >> 2) << s;
    return ra_snap565(c);
}

int ra_needs_outline(uint32_t fill, uint32_t bg)
{
    return !ra_meets(fill, bg, 3, 2);
}

uint16_t ra_to565(uint32_t rgb)
{
    uint32_t s = ra_snap565(rgb);
    return (uint16_t)(((s >> 19) << 11) | (((s >> 10) & 63) << 5) | ((s >> 3) & 31));
}

uint32_t ra_from565(uint16_t v)
{
    return ((uint32_t)((v >> 11) & 31) << 19) | ((uint32_t)((v >> 5) & 63) << 10) | ((uint32_t)(v & 31) << 3);
}

static uint32_t rd(const uint8_t *p)
{
    return ra_from565((uint16_t)((p[0] << 8) | p[1]));
}

int ra_theme_decode(const uint8_t *b, unsigned len, ra_theme_t *out)
{
    if (len != RA_WIRE_LEN) return -1;
    if (b[0] != RA_WIRE_VERSION) return -2;
    uint32_t *f[] = {&out->bg,    &out->s1,   &out->s2,     &out->s3,   &out->tx,    &out->tx2, &out->tx3,
                     &out->acc,   &out->onacc, &out->warm,  &out->onwarm, &out->bad, &out->onbad};
    const uint8_t *p = b + 1;
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++, p += 2)
        *f[i] = rd(p);
    for (unsigned i = 0; i < RA_PALETTE_N; i++, p += 6) {
        out->palette[i].fill = rd(p);
        out->palette[i].ink = rd(p + 2);
        out->palette[i].pressed = rd(p + 4);
    }
    out->id = RA_THEME_CUSTOM_ID;
    out->key = "custom";
    out->name = "Custom";
    return 0;
}
