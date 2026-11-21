#ifndef HP_REMOTE_UI_H
#define HP_REMOTE_UI_H

/* Page -> LVGL widgets (direction B "Cards"; tokens in theme.h). v2 pages draw by role (default / primary /
 * secondary / danger / nav), shape (card / row / pill / monogram) and icon (an icon-font glyph as an extra label: 32 px
 * on big cards, 24 in rows, pills and mid cards, 20 in dense grids); a button whose bind is the running activity is
 * teal ("active"); tiles draw off / on (warm) / unavailable ("n/a") / value, and "stale" while the link is lost.
 * Version 1 pages keep the old look and the two old guesses (title label in the bar, "Power Off" in red). The top 32 px are the status bar
 * (title, RF link, battery); the page grid fills the 240x288 content area below it. Cell edges use the
 * same integer mapping as hp_page_hit(), so a tapped pixel and the cell drawn under it always agree:
 * cell column c spans px [ceil(c*240/cols), ceil((c+1)*240/cols)), row r spans
 * [32 + ceil(r*288/rows), 32 + ceil((r+1)*288/rows)). Widgets are drawn inset inside their cell. */
#include "lvgl.h"
#include "remote/app/icons.h"
#include "remote/app/theme.h"
#include "remote/page/page.h"

#define RA_UI_PAGE_ERROR 0xFC    /* out of memory: error state, no widgets */
#define RA_UI_PAGE_RECEIVING 0xFD /* built-in pages use ids a real page cannot be mistaken for... */
#define RA_UI_PAGE_BOOT 0xFE
#define RA_UI_PAGE_FALLBACK 0xFF /* ...but tile_update on them finds no tile and is ignored */


/* Now Playing (now_playing, RF 29). flags 1 playing, 2 paused, 0 nothing. pos is as of `at` (ms, the app clock). */
typedef struct {
  uint8_t flags;
  uint16_t pos, dur;
  uint32_t cid;
  uint64_t at;
  char t1[49], t2[49];
} ra_np_t;

typedef struct {
  uint8_t page_theme; /* theme id the last v2 page asked for */
  hp_page_t page; /* what is on screen (boot / fallback / pushed); built-in pages have count 0 */
  lv_obj_t *obj[HP_PAGE_MAX_WIDGETS];
  lv_obj_t *lbl[HP_PAGE_MAX_WIDGETS];
  lv_obj_t *ico[HP_PAGE_MAX_WIDGETS];  /* v2: icon glyph label, else NULL */
  lv_obj_t *wcap[HP_PAGE_MAX_WIDGETS]; /* v2 tiles: caption (name, or "stale"), else NULL */
  uint32_t active;                     /* bit i: widget i is drawn active */
  uint8_t act_idx;                     /* running activity index and power, from state_update */
  int act_on;
  int fallback;   /* 1 while the "Hub unreachable" page is shown */
  int title_idx;  /* widget shown as the bar title instead of in the grid, or -1 */
  int pressed;    /* widget index drawn pressed, or -1 */
  /* state screens and the empty page: main message and caption labels (else NULL) */
  lv_obj_t *msg, *cap;
  int msg_bad;          /* msg drawn in the BAD colour (else TX) */
  lv_obj_t *err_msg;    /* the error overlay's headline */
  lv_obj_t *prog_fill;  /* receiving screen: progress bar fill */
  int recv_total;       /* receiving screen showing "k of N" (N), else 0 */
  /* status bar */
  lv_obj_t *bar, *title, *rf[3], *pct, *batt, *batt_fill, *back, *seq;
  int link_lost;  /* RF link indicator */
  int soc;        /* percent or -1 unknown */
  int charging;   /* external power present */
  lv_obj_t *toast;
  lv_obj_t *err;  /* hidden error overlay, created once at init */
  unsigned oom;   /* times a screen could not be built (memory) and the error state was shown */
  lv_timer_t *toast_timer;
  /* Now Playing card: np_row = grid row it may replace on this page (-1 none); np_idx = widget index of the card, -1 when
   * the card is not shown. The widgets of the replaced row are dropped from `page` (drawing and hit test). */
  ra_np_t np;
  int np_row, np_idx;
  uint32_t np_cid_drawn;  /* cid of the cover in the art slot, 0 = the Plex glyph */
  unsigned np_sec_drawn;  /* position (s) the time label shows */
  lv_obj_t *np_art, *np_t1, *np_t2, *np_gly, *np_tm, *np_bar, *np_fill;
} ra_ui_t;

void ra_ui_init(ra_ui_t *ui); /* needs an LVGL display; shows the boot page */
void ra_ui_show_page(ra_ui_t *ui, const hp_page_t *p); /* a v2 page also applies its theme preset */
/* The custom theme (99-byte wire form) for pages with theme id 7; until one is set such pages use the default preset.
 * 0 ok, -1 invalid (the previous one is kept). */
int ra_ui_set_custom_theme(const uint8_t *bytes, size_t n);
void ra_ui_show_boot(ra_ui_t *ui);      /* "Connecting" */
void ra_ui_show_receiving(ra_ui_t *ui); /* hub heard, no page yet */
/* "Receiving page k of N" with a progress bar; called again it only updates the texts and the bar. */
void ra_ui_show_receiving_n(ra_ui_t *ui, unsigned k, unsigned n);
void ra_ui_show_fallback(ra_ui_t *ui);  /* "Hub unreachable" */
/* 0 ok; <0 as hp_page_set_tile_text, or -HP_E_NOENT when page_id is not the page on screen. */
int ra_ui_tile_text(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, const char *text, size_t len);
/* tile_state: sets the tile's state (hp_tile_state_t) and text on the page on screen and redraws only that tile. */
int ra_ui_tile_state(ra_ui_t *ui, uint8_t page_id, uint8_t tile_id, uint8_t state, const char *text, size_t len);
/* Now Playing: the hub's latest state. 1 = the card must appear or go away (the caller re-shows the page), else the card
 * on screen (if any) is updated in place. The card replaces the first grid row of big cards (80 px: span 12 or 6, not
 * compact) of the page of the running activity (page act == activity + 1, power on) while flags != 0. */
int ra_ui_set_np(ra_ui_t *ui, const ra_np_t *np);
/* The cover for `cid` arrived (cover.h): redraw the art slot when the card shows that cid. */
void ra_ui_np_cover(ra_ui_t *ui, uint32_t cid);
/* The card's position and bar for `now` (ms): redraws only when the whole second changed; no-op while paused. */
void ra_ui_np_tick(ra_ui_t *ui, uint64_t now);
/* Ms to the next whole-second change of the position while the card shows a playing item, else UINT32_MAX. */
uint32_t ra_ui_np_next_ms(const ra_ui_t *ui, uint64_t now);
/* The card widget on screen is `w` (a tap on it is play / pause). */
int ra_ui_is_np(const ra_ui_t *ui, const hp_widget_t *w);
/* The running activity (index) and power from state_update: widgets whose bind matches turn teal; only those redraw. */
void ra_ui_set_activity(ra_ui_t *ui, uint8_t activity, int power);
/* Pixel box of the grid cell span of a widget (includes the page margin/gap area), same maths as the hit-tester. */
void ra_ui_widget_box(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2);
/* The rectangle a widget is actually drawn in: its cell box inset by the 8 px margin / gap. */
void ra_ui_widget_rect(const hp_page_t *p, const hp_widget_t *w, int *x1, int *y1, int *x2, int *y2);

/* Pressed feedback: draw the widget under (x,y) pressed (only that widget is invalidated); none for a
 * label, empty cell or the bar. ra_ui_release clears it. */
void ra_ui_press_at(ra_ui_t *ui, int x, int y);
void ra_ui_release(ra_ui_t *ui);

/* Status bar. */
void ra_ui_set_link(ra_ui_t *ui, int lost);
void ra_ui_set_battery(ra_ui_t *ui, int soc, int charging);
/* Non-modal toast over the page bottom; replaces a toast already up; dismissed after RA_TOAST_MS or
 * ra_ui_toast_dismiss(). line2 may be NULL. */
void ra_ui_toast(ra_ui_t *ui, const char *line1, const char *line2);
void ra_ui_toast_dismiss(ra_ui_t *ui);

/* Host-test hooks: fail the Nth object creation as if out of memory (-1 = off); skip the free-memory precheck. */
extern int ra_ui_test_fail_at, ra_ui_test_skip_precheck;

#endif
