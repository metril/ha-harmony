#define _POSIX_C_SOURCE 200809L
#include "remote/app/display.h"

#include <errno.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* One display per process: LVGL is a singleton here too. */
static uint8_t g_draw_buf[RA_SCREEN_W * RA_DRAW_ROWS * 2] __attribute__((aligned(64)));

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ra_display_note(ra_display_t *d, unsigned why, uint64_t input_ms) {
  d->why |= why;
  if (input_ms && !d->input_ms) d->input_ms = input_ms;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  ra_display_t *d = lv_display_get_user_data(disp);
  uint64_t t0 = mono_ns();
  if (a->y1 >= 0 && a->y2 < RA_SCREEN_H && a->x1 >= 0 && a->x2 < RA_SCREEN_W) { /* never write outside page 0 */
    size_t w = (size_t)(a->x2 - a->x1 + 1) * 2;
    uint8_t *dst = d->fb + (size_t)a->y1 * d->stride + (size_t)a->x1 * 2;
    if (w == d->stride) { /* full-width area: one copy */
      memcpy(dst, px, w * (size_t)(a->y2 - a->y1 + 1));
    } else {
      for (int32_t y = a->y1; y <= a->y2; y++) memcpy(dst + (size_t)(y - a->y1) * d->stride, px + (size_t)(y - a->y1) * w, w);
    }
    d->flush_px += (unsigned)((a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1));
    d->cyc_px += (unsigned)((a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1));
  }
  d->flushes++;
  d->cyc_flushes++;
  d->last_flush_ms = d->sys->ops.now_ms(d->sys->ops.ctx);
  d->cyc_flush_ns += mono_ns() - t0;
  if (lv_display_flush_is_last(disp)) d->frames++;
  lv_display_flush_ready(disp);
}

static void refr_cb(lv_event_t *e) {
  ra_display_t *d = lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
    d->cyc_flushes = d->cyc_px = 0;
    d->cyc_flush_ns = 0;
    d->cyc_start_ns = mono_ns();
    return;
  }
  if (!d->cyc_flushes) return; /* LVGL also opens a cycle when nothing is dirty: not a refresh */
  uint64_t total = mono_ns() - d->cyc_start_ns;
  d->last_flush_ns = d->cyc_flush_ns;
  d->last_render_ns = total > d->cyc_flush_ns ? total - d->cyc_flush_ns : 0;
  d->cycles++;
  if (d->ui_debug) {
    static const struct { unsigned bit; const char *name; } names[] = {
        {RA_WHY_PAGE, "page"}, {RA_WHY_PRESS, "press"}, {RA_WHY_RELEASE, "release"}, {RA_WHY_STATUS, "status"}, {RA_WHY_WAKE, "wake"}};
    char why[48] = "";
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
      if (d->why & names[i].bit) snprintf(why + strlen(why), sizeof why - strlen(why), "%s%s", *why ? "+" : "", names[i].name);
    if (!*why) strcpy(why, "other");
    uint64_t now = d->sys->ops.now_ms(d->sys->ops.ctx);
    char in[32] = "";
    if (d->input_ms) snprintf(in, sizeof in, " touch_to_flush=%ums", (unsigned)(d->last_flush_ms - d->input_ms));
    ra_logf(d->sys, "ui t=%llu flushes=%u px=%u render_us=%u flush_us=%u why=%s%s", (unsigned long long)now, d->cyc_flushes,
            d->cyc_px, (unsigned)(d->last_render_ns / 1000), (unsigned)(d->last_flush_ns / 1000), why, in);
  }
  d->why = 0;
  d->input_ms = 0;
}

int ra_display_init(ra_display_t *d, ra_sys_t *sys) {
  memset(d, 0, sizeof *d);
  d->sys = sys;
  if (ra_open_fb(sys) < 0) return -1;
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  memset(&v, 0, sizeof v);
  memset(&f, 0, sizeof f);
  if (ra_ioctl_fb(sys, FBIOGET_VSCREENINFO, &v) < 0 || ra_ioctl_fb(sys, FBIOGET_FSCREENINFO, &f) < 0) return -1;
  if (v.xres != RA_SCREEN_W || v.yres != RA_SCREEN_H || v.bits_per_pixel != 16 || v.red.offset != 11 ||
      v.red.length != 5 || v.green.offset != 5 || v.green.length != 6 || v.blue.offset != 0 ||
      v.blue.length != 5 || f.line_length < RA_SCREEN_W * 2u) {
    ra_logf(sys, "refusing to start: fb is %ux%u %ubpp r%u/%u g%u/%u b%u/%u, need 240x320 RGB565", v.xres, v.yres,
            v.bits_per_pixel, v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset,
            v.blue.length);
    return -2;
  }
  d->stride = f.line_length;
  d->fb_len = (size_t)f.line_length * RA_SCREEN_H;
  d->fb = ra_mmap_fb(sys, d->fb_len);
  if (!d->fb) return -1;
  if (ra_fb_unblank(sys) < 0) ra_logf(sys, "FBIOBLANK unblank failed (%d); the panel may stay dark", errno);

  d->disp = lv_display_create(RA_SCREEN_W, RA_SCREEN_H);
  lv_display_set_color_format(d->disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(d->disp, g_draw_buf, NULL, sizeof g_draw_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d->disp, flush_cb);
  lv_display_set_user_data(d->disp, d);
  lv_display_add_event_cb(d->disp, refr_cb, LV_EVENT_REFR_START, d);
  lv_display_add_event_cb(d->disp, refr_cb, LV_EVENT_REFR_READY, d);
  return 0;
}

void ra_display_destroy(ra_display_t *d) {
  if (d->disp) lv_display_delete(d->disp);
  if (d->fb) d->sys->ops.munmap(d->sys->ops.ctx, d->fb, d->fb_len);
  memset(d, 0, sizeof *d);
}
