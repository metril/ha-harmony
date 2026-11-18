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

#define KIND_RESERVED 0x8C /* widget kind bits 2, 3, 7 */
#define HAS_STYLE 0x10
#define HAS_ICON 0x20
#define HAS_BIND 0x40
#define STYLE_ICON_ONLY 0x20
#define PF_BACK 4

int hp_page_decode(const uint8_t *d, size_t len, hp_page_t *out) {
    if (len > HP_PAGE_MAX_BYTES) return -HP_E_TOOBIG;
    if (len < 1 || (d[0] != 1 && d[0] != 2)) return -HP_E_INVAL;
    int v2 = d[0] == 2;
    size_t off = v2 ? HP_PAGE_V2_HEADER : HP_PAGE_V1_HEADER;
    if (len < off) return -HP_E_INVAL;
    unsigned count = d[4];
    if (count > HP_PAGE_MAX_WIDGETS) return -HP_E_TOOBIG;
    hp_page_t p;
    memset(&p, 0, sizeof p);
    p.version = d[0];
    p.page_id = d[1];
    p.cols = d[2];
    p.rows = d[3];
    p.count = (uint8_t)count;
    if (v2) {
        unsigned pflags = d[7], tlen = d[10];
        if ((pflags & 3) == 3 || (pflags & ~(3u | PF_BACK))) return -HP_E_INVAL;
        if (tlen > HP_PAGE_MAX_TITLE) return -HP_E_TOOBIG;
        if (len - off < tlen || !utf8_valid(d + off, tlen)) return -HP_E_INVAL;
        memcpy(p.title, d + off, tlen);
        off += tlen;
        p.gen = d[5];
        p.total = d[6];
        p.kind = (uint8_t)(pflags & 3);
        p.back = (pflags & PF_BACK) != 0;
        p.seq_idx = d[8] >> 4;
        p.seq_cnt = d[8] & 15;
        p.act = d[9];
    }
    for (unsigned i = 0; i < count; i++) {
        if (len - off < 5) return -HP_E_INVAL;
        hp_widget_t *w = &p.widgets[i];
        unsigned kind = d[off];
        if (v2 ? (kind & KIND_RESERVED) || (kind & 3) == 3 : kind > HP_WIDGET_LABEL) return -HP_E_INVAL;
        w->kind = (uint8_t)(kind & 3);
        w->id = d[off + 1];
        w->x = d[off + 2] >> 4;
        w->y = d[off + 2] & 15;
        w->w = d[off + 3] >> 4;
        w->h = d[off + 3] & 15;
        off += 4;
        if (v2) {
            size_t need = 1u + !!(kind & HAS_STYLE) + !!(kind & HAS_ICON) + !!(kind & HAS_BIND);
            if (len - off < need) return -HP_E_INVAL;
            if (kind & HAS_STYLE) {
                unsigned s = d[off++];
                if ((s & 7) > HP_ROLE_NAV || (s & 0xC0)) return -HP_E_INVAL;
                w->role = (uint8_t)(s & 7);
                w->shape = (uint8_t)((s >> 3) & 3);
                w->icon_only = (s & STYLE_ICON_ONLY) != 0;
            }
            if (kind & HAS_ICON) {
                w->icon = d[off++];
                if (w->icon == 255) return -HP_E_INVAL;
            }
            if (kind & HAS_BIND) {
                w->has_bind = 1;
                w->bind = d[off++];
            }
        }
        size_t ll = d[off++];
        if (ll > HP_PAGE_MAX_LABEL) return -HP_E_TOOBIG;
        if (len - off < ll) return -HP_E_INVAL;
        if (!utf8_valid(d + off, ll)) return -HP_E_INVAL;
        memcpy(w->label, d + off, ll);
        off += ll;
    }
    if (off != len) return -HP_E_INVAL;
    /* semantic checks, after the whole page parsed (same order as the Python codec) */
    if (p.cols < 1 || p.cols > HP_PAGE_MAX_GRID || p.rows < 1 || p.rows > HP_PAGE_MAX_GRID) return -HP_E_INVAL;
    if (v2) {
        if (p.total < 1 || p.page_id >= p.total) return -HP_E_INVAL;
        if (p.seq_idx || p.seq_cnt) {
            int i = p.seq_idx, c = p.seq_cnt;
            if (c < 2 || i < 1 || i > c) return -HP_E_INVAL;
            if (p.page_id - i + 1 < 0 || p.page_id + c - i >= p.total) return -HP_E_INVAL;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        const hp_widget_t *w = &p.widgets[i];
        if (w->id > 127 || !w->w || !w->h || w->x + w->w > p.cols || w->y + w->h > p.rows) return -HP_E_INVAL;
        if (w->role == HP_ROLE_NAV && (!w->has_bind || w->bind >= p.total)) return -HP_E_INVAL;
        for (unsigned j = 0; j < i; j++) {
            const hp_widget_t *o = &p.widgets[j];
            if (o->id == w->id) return -HP_E_INVAL;
            if (w->x < o->x + o->w && o->x < w->x + w->w && w->y < o->y + o->h && o->y < w->y + w->h)
                return -HP_E_INVAL;
        }
    }
    *out = p;
    return 0;
}

int hp_page_title(const hp_page_t *p) {
    if (p->version >= 2) return -1;
    for (unsigned i = 0; i < p->count; i++) {
        const hp_widget_t *w = &p->widgets[i];
        if (w->kind == HP_WIDGET_LABEL && w->x == 0 && w->y == 0 && w->w == p->cols && w->h == 1) return (int)i;
    }
    return -1;
}

int hp_page_row_shift(const hp_page_t *p) { return p->rows > 1 && hp_page_title(p) >= 0 ? 1 : 0; }

const hp_widget_t *hp_page_hit(const hp_page_t *p, int x_px, int y_px) {
    if (x_px < 0 || y_px < HP_PAGE_BAR_H || x_px >= HP_PAGE_SCREEN_W || y_px >= HP_PAGE_SCREEN_H) return NULL;
    int cx = x_px * p->cols / HP_PAGE_SCREEN_W, cy = (y_px - HP_PAGE_BAR_H) * (p->rows - hp_page_row_shift(p)) / HP_PAGE_CONTENT_H + hp_page_row_shift(p);
    for (unsigned i = 0; i < p->count; i++) {
        const hp_widget_t *w = &p->widgets[i];
        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h) return w;
    }
    return NULL;
}

size_t hp_text_cut(const char *s, size_t len, size_t max) {
    size_t n = 0;
    while (n < len) {
        size_t k = utf8_seq((const uint8_t *)s + n, len - n);
        if (!k || n + k > max) break;
        n += k;
    }
    return n;
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
