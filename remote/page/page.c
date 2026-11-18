#include "remote/page/page.h"

#include <string.h>

#include "hub/hp_errors.h"

/* Length of the valid UTF-8 sequence at s (0 if invalid or NUL). Rejects overlong, surrogates, > U+10FFFF. */
static size_t utf8_seq(const uint8_t *s, size_t n) {
    uint8_t c = s[0];
    size_t k;
    uint32_t cp, min;
    if (c < 0x80) return c ? 1 : 0;
    if (c >= 0xC2 && c <= 0xDF) { k = 2; cp = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 3; cp = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if (n < k) return 0;
    for (size_t i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return k;
}

static int utf8_valid(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        size_t k = utf8_seq(s + i, n - i);
        if (!k) return 0;
        i += k;
    }
    return 1;
}

int hp_page_decode(const uint8_t *d, size_t len, hp_page_t *out) {
    if (len > HP_PAGE_MAX_BYTES) return -HP_E_TOOBIG;
    if (len < 5 || d[0] != HP_PAGE_VERSION) return -HP_E_INVAL;
    uint8_t cols = d[2], rows = d[3], count = d[4];
    if (cols < 1 || cols > HP_PAGE_MAX_GRID || rows < 1 || rows > HP_PAGE_MAX_GRID) return -HP_E_INVAL;
    if (count > HP_PAGE_MAX_WIDGETS) return -HP_E_TOOBIG;
    hp_page_t p;
    memset(&p, 0, sizeof p);
    p.page_id = d[1];
    p.cols = cols;
    p.rows = rows;
    p.count = count;
    size_t off = 5;
    for (unsigned i = 0; i < count; i++) {
        if (len - off < 5) return -HP_E_INVAL;
        hp_widget_t *w = &p.widgets[i];
        w->kind = d[off];
        w->id = d[off + 1];
        w->x = d[off + 2] >> 4;
        w->y = d[off + 2] & 15;
        w->w = d[off + 3] >> 4;
        w->h = d[off + 3] & 15;
        size_t ll = d[off + 4];
        off += 5;
        if (w->kind > HP_WIDGET_LABEL || w->id > 127) return -HP_E_INVAL;
        if (ll > HP_PAGE_MAX_LABEL) return -HP_E_TOOBIG;
        if (len - off < ll) return -HP_E_INVAL;
        if (!utf8_valid(d + off, ll)) return -HP_E_INVAL;
        memcpy(w->label, d + off, ll);
        off += ll;
        if (!w->w || !w->h || w->x + w->w > cols || w->y + w->h > rows) return -HP_E_INVAL;
        for (unsigned j = 0; j < i; j++) {
            const hp_widget_t *o = &p.widgets[j];
            if (o->id == w->id) return -HP_E_INVAL;
            if (w->x < o->x + o->w && o->x < w->x + w->w && w->y < o->y + o->h && o->y < w->y + w->h)
                return -HP_E_INVAL;
        }
    }
    if (off != len) return -HP_E_INVAL;
    *out = p;
    return 0;
}

const hp_widget_t *hp_page_hit(const hp_page_t *p, int x_px, int y_px) {
    if (x_px < 0 || y_px < HP_PAGE_BAR_H || x_px >= HP_PAGE_SCREEN_W || y_px >= HP_PAGE_SCREEN_H) return NULL;
    int cx = x_px * p->cols / HP_PAGE_SCREEN_W, cy = (y_px - HP_PAGE_BAR_H) * p->rows / HP_PAGE_CONTENT_H;
    for (unsigned i = 0; i < p->count; i++) {
        const hp_widget_t *w = &p->widgets[i];
        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h) return w;
    }
    return NULL;
}

int hp_page_set_tile_text(hp_page_t *p, uint8_t tile_id, const char *text, size_t len) {
    hp_widget_t *w = NULL;
    for (unsigned i = 0; i < p->count; i++)
        if (p->widgets[i].id == tile_id && p->widgets[i].kind == HP_WIDGET_TILE) w = &p->widgets[i];
    if (!w) return -HP_E_NOENT;
    const uint8_t *s = (const uint8_t *)text;
    if (!utf8_valid(s, len)) return -HP_E_INVAL;
    size_t n = 0;
    while (n < len) { /* whole characters only */
        size_t k = utf8_seq(s + n, len - n);
        if (n + k > HP_PAGE_MAX_TEXT) break;
        n += k;
    }
    memcpy(w->text, text, n);
    w->text[n] = 0;
    return 0;
}
