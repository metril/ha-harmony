#include "remote/page/store.h"

#include <string.h>

#include "hub/hp_errors.h"

void hp_store_init(hp_store_t *s) { memset(s, 0, sizeof *s); }

static void drop_tiles(hp_store_t *s, int page) {
    for (unsigned i = 0; i < HP_STORE_TILES; i++)
        if (s->tiles[i].used && (page < 0 || s->tiles[i].page == page)) s->tiles[i].used = 0;
}

int hp_store_put(hp_store_t *s, const uint8_t *raw, size_t len) {
    hp_page_t p; /* scratch decode: the validation is the decoder's */
    int rc = hp_page_decode(raw, len, &p);
    if (rc < 0 || p.version != 2) return -HP_E_INVAL;
    if (p.page_id >= HP_STORE_MAX) return -HP_E_TOOBIG;
    int ret = HP_STORE_OK;
    if (s->have_gen && s->gen != p.gen) {
        bool had = s->count > 0;
        hp_store_init(s);
        if (had) ret = HP_STORE_CLEARED;
    }
    s->have_gen = true;
    s->gen = p.gen;
    s->total = p.total;
    hp_store_meta_t *m = &s->meta[p.page_id];
    if (!m->len) s->count++;
    memcpy(s->raw[p.page_id], raw, len);
    *m = (hp_store_meta_t){.len = (uint8_t)len, .kind = p.kind, .act = p.act, .seq_idx = p.seq_idx, .seq_cnt = p.seq_cnt,
                           .back = p.back};
    drop_tiles(s, p.page_id);
    return ret;
}

bool hp_store_same(const hp_store_t *s, const uint8_t *raw, size_t len) {
    if (len < HP_PAGE_V2_HEADER || raw[0] != 2 || raw[1] >= HP_STORE_MAX) return false;
    return s->have_gen && s->gen == raw[5] && s->meta[raw[1]].len == len && !memcmp(s->raw[raw[1]], raw, len);
}

bool hp_store_has(const hp_store_t *s, uint8_t id) { return id < HP_STORE_MAX && s->meta[id].len; }
unsigned hp_store_count(const hp_store_t *s) { return s->count; }
unsigned hp_store_need(const hp_store_t *s) { return s->total < HP_STORE_MAX ? s->total : HP_STORE_MAX; }
bool hp_store_complete(const hp_store_t *s) { return s->count && s->count >= hp_store_need(s); }

int hp_store_get(const hp_store_t *s, uint8_t id, hp_page_t *out) {
    if (!hp_store_has(s, id)) return -HP_E_NOENT;
    if (hp_page_decode(s->raw[id], s->meta[id].len, out) < 0) return -HP_E_INVAL; /* cannot happen: validated on put */
    for (unsigned i = 0; i < HP_STORE_TILES; i++) {
        const hp_store_tile_t *t = &s->tiles[i];
        if (!t->used || t->page != id) continue;
        if (hp_page_set_tile_text(out, t->tile, t->text, strlen(t->text)) == 0)
            for (unsigned k = 0; k < out->count; k++)
                if (out->widgets[k].id == t->tile && out->widgets[k].kind == HP_WIDGET_TILE) out->widgets[k].tstate = t->state;
    }
    return 0;
}

int hp_store_find_kind(const hp_store_t *s, uint8_t kind) {
    for (unsigned i = 0; i < HP_STORE_MAX; i++)
        if (s->meta[i].len && s->meta[i].kind == kind) return (int)i;
    return -1;
}

int hp_store_find_act(const hp_store_t *s, uint8_t act) {
    if (!act) return -1;
    for (unsigned i = 0; i < HP_STORE_MAX; i++)
        if (s->meta[i].len && s->meta[i].act == act) return (int)i;
    return -1;
}

int hp_store_start(const hp_store_t *s) {
    int h = hp_store_find_kind(s, HP_PAGE_KIND_HOME);
    if (h >= 0) return h;
    return hp_store_has(s, 0) ? 0 : -1;
}

int hp_store_tile(hp_store_t *s, uint8_t page, uint8_t tile, uint8_t state, const char *text, size_t len) {
    size_t n = hp_text_cut(text, len, HP_PAGE_MAX_TEXT);
    if (n < len && n < HP_PAGE_MAX_TEXT) { /* stopped before the limit: not a plain overflow, so bad UTF-8 */
        return -HP_E_INVAL;
    }
    hp_store_tile_t *t = NULL;
    for (unsigned i = 0; i < HP_STORE_TILES; i++)
        if (s->tiles[i].used && s->tiles[i].page == page && s->tiles[i].tile == tile) t = &s->tiles[i];
    for (unsigned i = 0; i < HP_STORE_TILES && !t; i++)
        if (!s->tiles[i].used) t = &s->tiles[i];
    if (!t) {
        t = &s->tiles[s->tile_next];
        s->tile_next = (uint8_t)((s->tile_next + 1) % HP_STORE_TILES);
    }
    t->used = 1;
    t->page = page;
    t->tile = tile;
    t->state = state;
    memcpy(t->text, text, n);
    t->text[n] = 0;
    return 0;
}
