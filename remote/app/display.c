#include "remote/app/display.h"

#include <errno.h>
#include <linux/fb.h>
#include <string.h>

/* One display per process: LVGL is a singleton here too. */
static uint8_t g_draw_buf[RA_SCREEN_W * RA_DRAW_ROWS * 2] __attribute__((aligned(64)));

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  ra_display_t *d = lv_display_get_user_data(disp);
  size_t w = (size_t)(a->x2 - a->x1 + 1) * 2;
  for (int32_t y = a->y1; y <= a->y2; y++) {
    if (y < 0 || y >= RA_SCREEN_H || a->x1 < 0 || a->x2 >= RA_SCREEN_W) continue; /* never write outside page 0 */
    memcpy(d->fb + (size_t)y * d->stride + (size_t)a->x1 * 2, px + (size_t)(y - a->y1) * w, w);
  }
  if (lv_display_flush_is_last(disp)) d->frames++;
  lv_display_flush_ready(disp);
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
  return 0;
}

void ra_display_destroy(ra_display_t *d) {
  if (d->disp) lv_display_delete(d->disp);
  if (d->fb) d->sys->ops.munmap(d->sys->ops.ctx, d->fb, d->fb_len);
  memset(d, 0, sizeof *d);
}
