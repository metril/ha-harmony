#ifndef HP_PAGE_H
#define HP_PAGE_H

/* Remote page model + decoder (format: docs/proto/page-format.md). No allocation. */
#include <stddef.h>
#include <stdint.h>

#define HP_PAGE_VERSION 2 /* highest version decoded; version 1 pages still decode (defaults for the v2 fields) */
#define HP_PAGE_MAX_TITLE 24
#define HP_PAGE_V1_HEADER 5
#define HP_PAGE_V2_HEADER 11
#define HP_PAGE_MAX_WIDGETS 32
#define HP_PAGE_MAX_LABEL 24
#define HP_PAGE_MAX_TEXT 31
/* 251, not 255: a page travels in one RF object, whose body is at most 254 bytes
 * (HP_RF_BODY_MAX), and page_push spends 3 of them on its own header. Asserted in
 * hub_handlers.c so the two cannot drift apart. */
#define HP_PAGE_MAX_BYTES 251
#define HP_PAGE_MAX_GRID 16
#define HP_PAGE_SCREEN_W 240
#define HP_PAGE_SCREEN_H 320
/* The top 32 px are the remote's status bar; the page grid fills the 240x288 content area below it. */
#define HP_PAGE_BAR_H 32
#define HP_PAGE_CONTENT_H (HP_PAGE_SCREEN_H - HP_PAGE_BAR_H)

typedef enum { HP_WIDGET_BUTTON = 0, HP_WIDGET_TILE = 1, HP_WIDGET_LABEL = 2 } hp_widget_kind_t;

typedef enum { HP_ROLE_DEFAULT = 0, HP_ROLE_PRIMARY, HP_ROLE_SECONDARY, HP_ROLE_DANGER, HP_ROLE_NAV } hp_role_t;
typedef enum { HP_SHAPE_CARD = 0, HP_SHAPE_ROW, HP_SHAPE_PILL, HP_SHAPE_MONOGRAM } hp_shape_t;
typedef enum { HP_PAGE_KIND_NORMAL = 0, HP_PAGE_KIND_HOME, HP_PAGE_KIND_FAVORITES } hp_page_kind_t;
/* Tile states (tile_state.state). */
typedef enum { HP_TILE_VALUE = 0, HP_TILE_OFF, HP_TILE_ON, HP_TILE_UNAVAILABLE } hp_tile_state_t;

/* Widget colour (v2 HAS_COLOR): none = the role default, palette = theme palette index 1..12, rgb = RGB565. */
typedef enum { HP_COLOR_NONE = 0, HP_COLOR_PALETTE, HP_COLOR_RGB } hp_color_kind_t;
#define HP_PAGE_PALETTE_MAX 12
#define HP_PAGE_THEME_CUSTOM 7 /* header theme id 7: a custom theme delivered separately; presets are 0..6 */
typedef struct {
    uint8_t kind;   /* hp_color_kind_t */
    uint16_t value; /* palette index (1..12) or RGB565 */
} hp_color_t;

typedef enum { HP_ALIGN_CENTER = 0, HP_ALIGN_LEFT = 1, HP_ALIGN_RIGHT = 2 } hp_align_t;

typedef struct {
    uint8_t kind, id, x, y, w, h;
    uint8_t role, shape, icon_only; /* v2; v1: default / card / 0 */
    uint8_t trailing;               /* nav row: chevron at the right edge */
    uint8_t align;                  /* hp_align_t: icon + label group alignment (kind bits 2-3) */
    uint8_t icon;                   /* 0 none, 1..159 stock, 160..254 custom */
    uint8_t has_bind, bind;         /* role nav: target page id; else the activity index that makes it "active" */
    uint8_t tstate;                 /* live tile state (hp_tile_state_t), not on the wire in the page */
    hp_color_t color_fill;          /* v2: fill colour; none for v1 */
    hp_color_t color_on;            /* v2, tiles only: fill when the tile is "on"; none otherwise */
    char label[HP_PAGE_MAX_LABEL + 1]; /* NUL-terminated */
    char text[HP_PAGE_MAX_TEXT + 1];   /* live tile text, NUL-terminated */
} hp_widget_t;

typedef struct {
    uint8_t version;            /* 1 or 2 */
    uint8_t page_id, cols, rows, count;
    uint8_t gen, total;         /* v2: config generation (hash low byte), pages in the set; v1: 0, 0 (unknown) */
    uint8_t kind, back;         /* v2: hp_page_kind_t, BACK flag */
    uint8_t seq_idx, seq_cnt;   /* v2: paged group position; 0/0 = not paged */
    uint8_t act;                /* v2: 0, or activity index + 1 */
    uint8_t theme;              /* v2: header theme preset id 0..7 (0 default, 7 custom); v1: 0 */
    char title[HP_PAGE_MAX_TITLE + 1]; /* v2 header title, "" = none */
    hp_widget_t widgets[HP_PAGE_MAX_WIDGETS];
} hp_page_t;

/* v1 and v2 (docs/plans/2026-11-18-page-format-v2.md). 0 ok; -HP_E_INVAL malformed (version, truncated/trailing,
 * reserved bits, kind, UTF-8, bounds, zero size, overlap, duplicate id, bad paging / nav target / total);
 * -HP_E_TOOBIG too many widgets, long label or title, or len > HP_PAGE_MAX_BYTES (251). *out is only written
 * on success. Same verdicts as harmony_core/page_codec.py (tests/vectors/pages.json). */
int hp_page_decode(const uint8_t *data, size_t len, hp_page_t *out);

/* v1 ONLY (v2 pages carry the title in the header: -1 / 0 for them).
 * Index of the title label (a label at x=0,y=0 spanning all columns with h=1), or -1. The remote shows it in
 * the status bar; the other rows are then laid out over the whole 240x288 area as if the page had rows-1 rows
 * (hp_page_row_shift() = 1). Without a title, or with a 1-row page, the shift is 0. */
int hp_page_title(const hp_page_t *p);
int hp_page_row_shift(const hp_page_t *p);

/* Widget under pixel (x,y), or NULL (also for pixels outside the screen and in the status bar). */
const hp_widget_t *hp_page_hit(const hp_page_t *p, int x_px, int y_px);

/* Length of the longest prefix of s (len bytes) of whole valid UTF-8 characters that fits in max bytes. */
size_t hp_text_cut(const char *s, size_t len, size_t max);

/* Set live text of tile `tile_id`: truncated to HP_PAGE_MAX_TEXT bytes at a
 * UTF-8 boundary. 0 ok; -HP_E_NOENT no such tile; -HP_E_INVAL bad UTF-8 / NUL. */
int hp_page_set_tile_text(hp_page_t *p, uint8_t tile_id, const char *text, size_t len);

#endif
