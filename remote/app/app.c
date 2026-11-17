#include "remote/app/app.h"

#include <errno.h>
#include <string.h>

#include "hub/hp_errors.h"

#define LINK_POLL_MS 100 /* upper bound on the select() timeout: the link has no fd to wait on yet */
#define LINK_EVENTS_PER_STEP 8

static int null_send(void *ctx, uint8_t code, uint8_t action) {
  (void)ctx; (void)code; (void)action;
  return 0;
}
static int null_poll(void *ctx, uint64_t now, app_link_event_t *ev) {
  (void)ctx; (void)now; (void)ev;
  return 0;
}
const app_link_t app_link_null = {.send_key = null_send, .poll = null_poll};

/* LVGL's tick source (it keeps no context pointer). One app per process, so this is fine. */
static ra_sys_t *g_tick_sys;
static uint32_t tick_cb(void) { return (uint32_t)g_tick_sys->ops.now_ms(g_tick_sys->ops.ctx); }

static void sink_emit(void *ctx, uint8_t code, uint8_t action) {
  app_t *a = ctx;
  if (a->link.send_key(a->link.ctx, code, action) < 0) ra_logf(&a->sys, "send_key %u/%u failed", code, action);
}
static bool sink_gate(void *ctx, uint64_t now) {
  app_t *a = ctx;
  return ra_power_input(&a->power, now);
}
static void sink_activity(void *ctx, uint64_t now) {
  app_t *a = ctx;
  ra_power_activity(&a->power, now);
}
static const hp_page_t *sink_page(void *ctx) {
  app_t *a = ctx;
  return &a->ui.page;
}

int app_init(app_t *a, const ra_ops_t *ops, const app_link_t *link, const char *bootcount_path) {
  memset(a, 0, sizeof *a);
  ra_sys_init(&a->sys, ops, bootcount_path);
  a->link = link ? *link : app_link_null;
  g_tick_sys = &a->sys;
  lv_init();
  lv_tick_set_cb(tick_cb);
  int r = ra_display_init(&a->display, &a->sys);
  if (r < 0) {
    lv_deinit();
    return r;
  }
  ra_ui_init(&a->ui);
  ra_input_sink_t sink = {.ctx = a, .emit = sink_emit, .gate = sink_gate, .activity = sink_activity, .page = sink_page};
  if (ra_input_open(&a->input, &a->sys, &sink) < 0) {
    ra_logf(&a->sys, "cannot open input devices");
    ra_display_destroy(&a->display);
    lv_deinit();
    return -1;
  }
  a->input_open = true;
  ra_power_init(&a->power, &a->sys, a->sys.ops.now_ms(a->sys.ops.ctx));
  return 0;
}

static void handle_link_event(app_t *a, const app_link_event_t *ev) {
  switch (ev->kind) {
  case APP_LINK_EV_PAGE_PUSH: {
    hp_page_t p;
    int r = hp_page_decode(ev->data, ev->len, &p);
    if (r < 0) ra_logf(&a->sys, "page_push rejected (%d)", r);
    else ra_ui_show_page(&a->ui, &p);
    break;
  }
  case APP_LINK_EV_TILE_UPDATE:
    if (ra_ui_tile_text(&a->ui, ev->page, ev->tile, ev->text, strlen(ev->text)) < 0)
      ra_logf(&a->sys, "tile_update %u/%u ignored", ev->page, ev->tile);
    break;
  case APP_LINK_EV_STATE_UPDATE:
    a->state_updates++;
    a->last_activity = ev->activity;
    a->last_power = ev->power;
    break;
  case APP_LINK_EV_FAILED:
    if (!a->ui.fallback) ra_ui_show_fallback(&a->ui);
    break;
  case APP_LINK_EV_NONE:
    break;
  }
}

void app_step(app_t *a, unsigned max_wait_ms) {
  uint64_t now = a->sys.ops.now_ms(a->sys.ops.ctx);
  uint32_t lv_ms = lv_timer_handler();
  uint32_t t = max_wait_ms < LINK_POLL_MS ? max_wait_ms : LINK_POLL_MS;
  uint32_t d[3] = {lv_ms, ra_power_next_ms(&a->power, now), ra_input_next_ms(&a->input, now)};
  for (int i = 0; i < 3; i++)
    if (d[i] < t) t = d[i];
  int ready[2] = {0, 0};
  int w = a->sys.ops.wait(a->sys.ops.ctx, a->input.fd, 2, t, ready);
  now = a->sys.ops.now_ms(a->sys.ops.ctx);
  if (w > 0)
    for (int i = 0; i < 2; i++)
      if (ready[i]) ra_input_read(&a->input, i, now);
  ra_input_tick(&a->input, now);
  ra_power_tick(&a->power, now);
  app_link_event_t ev;
  for (int i = 0; i < LINK_EVENTS_PER_STEP && a->link.poll(a->link.ctx, now, &ev) > 0; i++) handle_link_event(a, &ev);
  lv_timer_handler(); /* render what changed */
  if (!a->healthy_written && a->input_open && a->display.frames >= 1 && ra_write_bootcount(&a->sys) == 0)
    a->healthy_written = true;
}

void app_destroy(app_t *a) {
  ra_input_close(&a->input);
  ra_display_destroy(&a->display);
  lv_deinit();
  g_tick_sys = NULL;
}
