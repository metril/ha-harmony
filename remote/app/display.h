#ifndef HP_REMOTE_DISPLAY_H
#define HP_REMOTE_DISPLAY_H

/* LVGL display on /dev/fb0: partial render (1/10 screen buffer), flushed by memcpy into page 0
 * of the mmapped framebuffer (no y-panning yet). */
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "remote/app/sys.h"

#define RA_SCREEN_W 240
#define RA_SCREEN_H 320
#define RA_DRAW_ROWS 32 /* draw buffer = 240 x 32 px (1/10 screen) */

typedef struct {
  ra_sys_t *sys;
  uint8_t *fb; /* page 0 of the mapping */
  size_t fb_len;
  uint32_t stride; /* bytes per fb line */
  lv_display_t *disp;
  unsigned frames; /* completed refreshes (last flush of a refresh) */
} ra_display_t;

/* 0 ok. -1 cannot open/query/map; -2 the panel is not 240x320 RGB565 (the app refuses to run). */
int ra_display_init(ra_display_t *d, ra_sys_t *sys);
void ra_display_destroy(ra_display_t *d);

#endif
