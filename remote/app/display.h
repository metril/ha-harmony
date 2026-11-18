#ifndef HP_REMOTE_DISPLAY_H
#define HP_REMOTE_DISPLAY_H

/* LVGL display on /dev/fb0: partial render into a cached-RAM draw buffer, flushed by memcpy into page 0
 * of the mmapped framebuffer (no y-panning: FBIOPAN is outside the syscall allow-list). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "remote/app/sys.h"

#define RA_SCREEN_W 240
#define RA_SCREEN_H 320
#define RA_DRAW_ROWS RA_SCREEN_H /* draw buffer = the whole screen (150 KiB of cached RAM, ~0.3 % of the ~50 MB free):
                                  * a full redraw renders once and flushes once, not in 10 strips over the visible page.
                                  * Not the fb's page 1: that mapping is uncached, and LVGL reads the buffer when blending. */

/* Why a refresh cycle happened, for --ui-debug (OR-ed; cleared when the cycle ends). */
#define RA_WHY_PAGE 1u    /* a page push or a state screen */
#define RA_WHY_PRESS 2u   /* touch down */
#define RA_WHY_RELEASE 4u /* touch up */
#define RA_WHY_STATUS 8u  /* status bar (battery, link) or tile text */
#define RA_WHY_WAKE 16u   /* the screen came back on */

typedef struct {
  ra_sys_t *sys;
  uint8_t *fb; /* page 0 of the mapping */
  size_t fb_len;
  uint32_t stride; /* bytes per fb line */
  lv_display_t *disp;
  unsigned frames; /* completed refreshes (last flush of a refresh) */
  /* Instrumentation (always compiled; the stderr line is printed only with ui_debug). */
  bool ui_debug;
  unsigned why;          /* RA_WHY_* since the last cycle ended */
  uint64_t input_ms;     /* now_ms of the input event behind the pending refresh, 0 = none */
  uint64_t last_flush_ms; /* now_ms of the most recent flush */
  unsigned flushes, flush_px; /* totals since init */
  unsigned cycles;            /* refresh cycles since init */
  unsigned cyc_flushes, cyc_px; /* the cycle in progress / the last finished one */
  uint64_t cyc_start_ns, cyc_flush_ns, last_render_ns, last_flush_ns;
} ra_display_t;

/* Record why the next refresh happens; `input_ms` (0 = none) is the event's timestamp. */
void ra_display_note(ra_display_t *d, unsigned why, uint64_t input_ms);

/* 0 ok. -1 cannot open/query/map; -2 the panel is not 240x320 RGB565 (the app refuses to run). */
int ra_display_init(ra_display_t *d, ra_sys_t *sys);
void ra_display_destroy(ra_display_t *d);

#endif
