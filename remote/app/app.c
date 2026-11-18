#include "remote/app/app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "hub/hp_errors.h"

#define LINK_POLL_MS 100 /* select() timeout cap for a link with no fd to wait on */
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
  bool pass = ra_power_input(&a->power, now);
  if (pass) ra_ui_toast_dismiss(&a->ui); /* any touch or key clears the toast */
  return pass;
}
static void sink_touch(void *ctx, int x, int y, bool down) {
  app_t *a = ctx;
  if (down) ra_ui_press_at(&a->ui, x, y);
  else ra_ui_release(&a->ui);
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
  ra_input_sink_t sink = {.ctx = a, .emit = sink_emit, .gate = sink_gate, .activity = sink_activity, .page = sink_page,
                       .touch = sink_touch};
  if (ra_input_open(&a->input, &a->sys, &sink) < 0) {
    ra_logf(&a->sys, "cannot open input devices");
    ra_display_destroy(&a->display);
    lv_deinit();
    return -1;
  }
  a->input_open = true;
  ra_power_init(&a->power, &a->sys, a->sys.ops.now_ms(a->sys.ops.ctx));
  a->last_rx_ms = a->sys.ops.now_ms(a->sys.ops.ctx);
  return 0;
}

static void heard(app_t *a, uint64_t now) {
  a->last_rx_ms = now;
  a->fail_run = 0;
  ra_ui_set_link(&a->ui, 0);
}

static void handle_link_event(app_t *a, const app_link_event_t *ev, uint64_t now) {
  switch (ev->kind) {
  case APP_LINK_EV_PAGE_PUSH: {
    hp_page_t p;
    heard(a, now);
    int r = hp_page_decode(ev->data, ev->len, &p);
    if (r < 0) {
      ra_logf(&a->sys, "page_push rejected (%d)", r);
    } else {
      a->saved = p;
      a->have_saved = true;
      ra_ui_show_page(&a->ui, &p);
    }
    break;
  }
  case APP_LINK_EV_TILE_UPDATE:
    heard(a, now);
    if (a->have_saved && a->saved.page_id == ev->page) (void)hp_page_set_tile_text(&a->saved, ev->tile, ev->text, strlen(ev->text));
    if (ra_ui_tile_text(&a->ui, ev->page, ev->tile, ev->text, strlen(ev->text)) < 0)
      ra_logf(&a->sys, "tile_update %u/%u ignored", ev->page, ev->tile);
    break;
  case APP_LINK_EV_STATE_UPDATE:
    heard(a, now);
    a->state_updates++;
    a->last_activity = ev->activity;
    a->last_power = ev->power;
    if (!a->have_saved && !a->ui.fallback && a->ui.page.page_id == RA_UI_PAGE_BOOT) ra_ui_show_receiving(&a->ui);
    break;
  case APP_LINK_EV_ALIVE:
    heard(a, now);
    if (a->ui.fallback) { /* the hub is back: the last real page, not a blank wait for the next push */
      if (a->have_saved) ra_ui_show_page(&a->ui, &a->saved);
      else ra_ui_show_receiving(&a->ui);
    } else if (!a->have_saved && a->ui.page.page_id == RA_UI_PAGE_BOOT) {
      ra_ui_show_receiving(&a->ui); /* the hub answers, its pages are on the way */
    }
    break;
  case APP_LINK_EV_FAILED:
    a->fail_run++;
    ra_ui_set_link(&a->ui, 1);
    if (!a->ui.fallback &&
        (ev->fatal || (a->fail_run >= APP_FAIL_COUNT && now - a->last_rx_ms >= APP_FAIL_SILENCE_MS)))
      ra_ui_show_fallback(&a->ui);
    break;
  case APP_LINK_EV_NONE:
    break;
  }
}

void app_step(app_t *a, unsigned max_wait_ms) {
  uint64_t now = a->sys.ops.now_ms(a->sys.ops.ctx);
  uint32_t lv_ms = lv_timer_handler();
  int fds[3] = {a->input.fd[0], a->input.fd[1], -1};
  int nfd = 2;
  int lfd = a->link.get_fd ? a->link.get_fd(a->link.ctx) : -1;
  if (lfd >= 0) fds[nfd++] = lfd;
  uint32_t cap = lfd >= 0 ? APP_LINK_IDLE_WAIT_MS : LINK_POLL_MS;
  uint32_t t = max_wait_ms < cap ? max_wait_ms : cap;
  uint32_t d[4] = {lv_ms, ra_power_next_ms(&a->power, now), ra_input_next_ms(&a->input, now),
                   a->link.next_ms ? a->link.next_ms(a->link.ctx, now) : UINT32_MAX};
  for (int i = 0; i < 4; i++)
    if (d[i] < t) t = d[i];
  int ready[3] = {0, 0, 0};
  int w = a->sys.ops.wait(a->sys.ops.ctx, fds, nfd, t, ready);
  now = a->sys.ops.now_ms(a->sys.ops.ctx);
  if (w > 0)
    for (int i = 0; i < 2; i++)
      if (ready[i]) ra_input_read(&a->input, i, now);
  ra_input_tick(&a->input, now);
  ra_power_tick(&a->power, now);
  if (a->power.batt_polls != a->last_batt_polls) {
    a->last_batt_polls = a->power.batt_polls;
    ra_ui_set_battery(&a->ui, a->power.soc, a->power.external_power);
    bool low = a->power.soc <= RA_LOWBATT_PCT && !a->power.external_power;
    if (low && !a->lowbatt_toasted) {
      char b[24];
      snprintf(b, sizeof b, "Battery %d%%", a->power.soc);
      ra_ui_toast(&a->ui, b, "Place on the charger");
    }
    a->lowbatt_toasted = low;
  }
  bool awake = a->power.state != RA_PW_OFF;
  if (a->link.set_awake && (!a->awake_known || awake != a->last_awake)) a->link.set_awake(a->link.ctx, awake);
  a->awake_known = true;
  a->last_awake = awake;
  app_link_event_t ev;
  for (int i = 0; i < LINK_EVENTS_PER_STEP && a->link.poll(a->link.ctx, now, &ev) > 0; i++) handle_link_event(a, &ev, now);
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
