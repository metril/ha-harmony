#ifndef HP_PAGE_STORE_H
#define HP_PAGE_STORE_H

/* The remote's page store (docs/plans/2026-11-18-page-format-v2.md, section 4): up to 32 v2 pages keyed by page id,
 * held as RAW wire bytes (<= 251 each) and decoded on show: decoding costs microseconds, so keeping decoded models
 * (2.5 KB each, 80 KB for 32) would be pure waste. A page with a new gen clears the store. Live tile state
 * (tile_update / tile_state) is kept apart in a small overlay, so it survives while a page is not on screen and is
 * applied whenever the page is decoded. No allocation, no LVGL. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remote/page/page.h"

#define HP_STORE_MAX 32  /* page ids 0..31 can be stored; a config has at most 32 pages */
#define HP_STORE_TILES 32 /* tile overlay entries; the oldest is recycled when full */

#define HP_STORE_OK 0
#define HP_STORE_CLEARED 1 /* stored, and the pages of an older gen were dropped */

typedef struct {
    uint8_t len; /* 0 = absent */
    uint8_t kind, act, seq_idx, seq_cnt, back;
} hp_store_meta_t;

typedef struct {
    uint8_t used, page, tile, state;
    char text[HP_PAGE_MAX_TEXT + 1];
} hp_store_tile_t;

typedef struct {
    bool have_gen;
    uint8_t gen, total, count; /* count = distinct pages held */
    hp_store_meta_t meta[HP_STORE_MAX];
    uint8_t raw[HP_STORE_MAX][HP_PAGE_MAX_BYTES];
    hp_store_tile_t tiles[HP_STORE_TILES];
    uint8_t tile_next;
} hp_store_t;

void hp_store_init(hp_store_t *);
/* Validates `raw` with the page decoder and stores it. HP_STORE_OK / HP_STORE_CLEARED; -HP_E_INVAL (bad page, or not
 * version 2); -HP_E_TOOBIG (page id or total > HP_STORE_MAX: it cannot be held). A changed gen OR total clears the store first (gen is one byte). Storing a page drops its tile overlay. */
int hp_store_put(hp_store_t *, const uint8_t *raw, size_t len);
bool hp_store_has(const hp_store_t *, uint8_t id);
/* The stored bytes of page `id` are exactly raw[0..len) (and the gen matches): a re-push that changes nothing. */
bool hp_store_same(const hp_store_t *, const uint8_t *raw, size_t len);
unsigned hp_store_count(const hp_store_t *);
/* Pages the set needs before it is complete: min(total, HP_STORE_MAX); 0 until a page arrived. */
unsigned hp_store_need(const hp_store_t *);
bool hp_store_complete(const hp_store_t *);
/* Decodes page `id` into *out and applies its tile overlay. 0; -HP_E_NOENT if absent. */
int hp_store_get(const hp_store_t *, uint8_t id, hp_page_t *out);
/* Lowest id of a page of that kind / that `act` value (activity index + 1), or -1. */
int hp_store_find_kind(const hp_store_t *, uint8_t kind);
int hp_store_find_act(const hp_store_t *, uint8_t act);
/* The page shown first: the home page, else page 0, else the lowest stored id (an incomplete set); -1 if none. */
int hp_store_start(const hp_store_t *);
/* Records live state for a tile (state: hp_tile_state_t). 0; -HP_E_INVAL bad UTF-8. */
int hp_store_tile(hp_store_t *, uint8_t page, uint8_t tile, uint8_t state, const char *text, size_t len);

#endif
