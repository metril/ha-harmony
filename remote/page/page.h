#ifndef HP_PAGE_H
#define HP_PAGE_H

/* Remote page model + decoder (format: docs/proto/page-format.md). No allocation. */
#include <stddef.h>
#include <stdint.h>

#define HP_PAGE_VERSION 1
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

typedef struct {
    uint8_t kind, id, x, y, w, h;
    char label[HP_PAGE_MAX_LABEL + 1]; /* NUL-terminated */
    char text[HP_PAGE_MAX_TEXT + 1];   /* live tile text, NUL-terminated */
} hp_widget_t;

typedef struct {
    uint8_t page_id, cols, rows, count;
    hp_widget_t widgets[HP_PAGE_MAX_WIDGETS];
} hp_page_t;

/* 0 ok; -HP_E_INVAL malformed (version, truncated/trailing, kind, UTF-8, bounds,
 * zero size, overlap, duplicate id); -HP_E_TOOBIG too many widgets, long label
 * or len > HP_PAGE_MAX_BYTES (251). *out is only written on success. */
int hp_page_decode(const uint8_t *data, size_t len, hp_page_t *out);

/* Widget under pixel (x,y), or NULL (also for pixels outside the screen and in the status bar). */
const hp_widget_t *hp_page_hit(const hp_page_t *p, int x_px, int y_px);

/* Set live text of tile `tile_id`: truncated to HP_PAGE_MAX_TEXT bytes at a
 * UTF-8 boundary. 0 ok; -HP_E_NOENT no such tile; -HP_E_INVAL bad UTF-8 / NUL. */
int hp_page_set_tile_text(hp_page_t *p, uint8_t tile_id, const char *text, size_t len);

#endif
