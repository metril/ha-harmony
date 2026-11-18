#include "remote/app/app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "hub/hp_errors.h"
#include "proto/gen/harmony_proto.h"

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

/* ---- version 2: page store, navigation, local keys ---- */

/* Renders the page `cur` from the store, or the "Receiving page k of N" screen while the set is incomplete or the page
 * has not arrived yet (it resolves when it does: see PAGE_PUSH). */
static void show_cur(app_t *a) {
  unsigned have = hp_store_count(&a->store), need = hp_store_need(&a->store);
  ra_display_note(&a->display, RA_WHY_PAGE, 0);
  hp_page_t p;
  if (hp_store_complete(&a->store) && a->cur >= 0 && hp_store_get(&a->store, (uint8_t)a->cur, &p) == 0) {
    ra_ui_show_page(&a->ui, &p);
    return;
  }
  if (a->cur >= 0 && (unsigned)a->cur >= need) need = (unsigned)a->cur + 1; /* a page past the set: still on its way */
  if (have >= need) need = have + 1;
  ra_ui_show_receiving_n(&a->ui, have, need);
}

static void goto_page(app_t *a, int id) {
  if (id < 0 || id == a->cur) return;
  a->cur = id;
  show_cur(a);
}

/* A stored page is on screen. */
static bool on_stored_page(const app_t *a) {
  return a->v2 && a->cur >= 0 && a->ui.page.version == 2 && a->ui.page.page_id == a->cur;
}

static void go_home(app_t *a) { goto_page(a, hp_store_start(&a->store)); }

static void page_step(app_t *a, int dir) {
  const hp_page_t *p = &a->ui.page;
  if (dir > 0 && p->seq_idx < p->seq_cnt) goto_page(a, a->cur + 1);
  else if (dir < 0 && p->seq_idx > 1) goto_page(a, a->cur - 1);
}

/* Hard keys the app acts on itself. Returns true when the key is consumed (not forwarded). Up / Down page a paged
 * page and Exit leaves a BACK page for Home: those never go to the hub. Home / Favorites switch to the page of that kind
 * if there is one, and are forwarded as well. A key consumed at press keeps its hold and release consumed too. */
static bool local_key(app_t *a, uint8_t code, uint8_t action) {
  int bit = code == a->key_up ? 0 : code == a->key_down ? 1 : code == a->key_exit ? 2 : -1;
  if (bit >= 0 && action != HP_KEY_ACTION_PRESS) {
    bool sw = (a->swallowed >> bit) & 1;
    if (action == HP_KEY_ACTION_RELEASE) a->swallowed &= (uint8_t)~(1u << bit);
    return sw;
  }
  if (!a->v2 || !on_stored_page(a)) return false;
  const hp_page_t *p = &a->ui.page;
  if (bit == 0 || bit == 1) {
    if (p->seq_cnt < 2) return false;
    a->swallowed |= (uint8_t)(1u << bit);
    page_step(a, bit == 1 ? 1 : -1);
    return true;
  }
  if (bit == 2) {
    if (!p->back) return false;
    a->swallowed |= 4;
    go_home(a);
    return true;
  }
  if (code == a->key_home || code == a->key_fav) {
    if (action == HP_KEY_ACTION_PRESS)
      goto_page(a, hp_store_find_kind(&a->store, code == a->key_home ? HP_PAGE_KIND_HOME : HP_PAGE_KIND_FAVORITES));
  }
  return false;
}

static bool sink_widget(void *ctx, const hp_widget_t *w) {
  app_t *a = ctx;
  if (!a->v2 || !on_stored_page(a) || w->role != HP_ROLE_NAV || !w->has_bind) return false;
  goto_page(a, w->bind);
  return true;
}

static void sink_bar(void *ctx, int x, int y) {
  app_t *a = ctx;
  (void)y;
  if (on_stored_page(a) && a->ui.page.back && x < 40) go_home(a); /* the back chevron */
}

static void sink_emit(void *ctx, uint8_t code, uint8_t action) {
  app_t *a = ctx;
  if (local_key(a, code, action)) return;
  if (a->ntx < sizeof a->tx / sizeof a->tx[0]) { /* the radio write can block: the pixels go out first (send_pending) */
    a->tx[a->ntx].code = code;
    a->tx[a->ntx++].action = action;
    return;
  }
  if (a->link.send_key(a->link.ctx, code, action) < 0) ra_logf(&a->sys, "send_key %u/%u failed", code, action);
}
static void send_pending(app_t *a) {
  for (unsigned i = 0; i < a->ntx; i++)
    if (a->link.send_key(a->link.ctx, a->tx[i].code, a->tx[i].action) < 0)
      ra_logf(&a->sys, "send_key %u/%u failed", a->tx[i].code, a->tx[i].action);
  a->ntx = 0;
}
static bool sink_gate(void *ctx, uint64_t now) {
  app_t *a = ctx;
  bool pass = ra_power_input(&a->power, now);
  if (pass) ra_ui_toast_dismiss(&a->ui); /* any touch or key clears the toast */
  return pass;
}
static void sink_touch(void *ctx, int x, int y, bool down) {
  app_t *a = ctx;
  ra_display_note(&a->display, down ? RA_WHY_PRESS : RA_WHY_RELEASE, a->input_ms);
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
  a->cur = -1;
  const hp_hwkey_t *k;
  a->key_up = (k = hp_hwkey_by_name("Up")) ? k->id : HP_KEY_NONE;
  a->key_down = (k = hp_hwkey_by_name("Down")) ? k->id : HP_KEY_NONE;
  a->key_exit = (k = hp_hwkey_by_name("Exit")) ? k->id : HP_KEY_NONE;
  a->key_home = (k = hp_hwkey_by_name("Home")) ? k->id : HP_KEY_NONE;
  a->key_fav = (k = hp_hwkey_by_name("Favorites")) ? k->id : HP_KEY_NONE;
  ra_input_sink_t sink = {.ctx = a, .emit = sink_emit, .gate = sink_gate, .activity = sink_activity, .page = sink_page,
                       .touch = sink_touch, .widget = sink_widget, .bar = sink_bar};
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

/* A version 2 page: into the store (a new gen clears it). While the set is incomplete the screen is "Receiving page k of
 * N"; when it completes the home page (else page 0, or the running activity's page) is shown; afterwards a page is
 * re-rendered only when the one on screen was replaced, or the one being waited for arrived. An identical re-push changes
 * nothing at all. */
static void push_v2(app_t *a, const uint8_t *raw, size_t len) {
  if (hp_store_same(&a->store, raw, len)) return;
  int r = hp_store_put(&a->store, raw, len);
  if (r < 0) {
    ra_logf(&a->sys, "page_push rejected (%d)", r);
    return;
  }
  if (!a->v2) {
    a->v2 = true;
    a->have_saved = false;
    a->cur = -1;
  }
  if (r == HP_STORE_CLEARED) a->cur = -1;
  bool complete = hp_store_complete(&a->store);
  if (!complete) {
    if (a->ui.page.version != 2 || a->ui.page.page_id != a->cur || a->cur < 0) show_cur(a); /* progress */
    return;
  }
  if (a->cur < 0) { /* the set just completed: pick what to show */
    a->cur = hp_store_start(&a->store);
    if (a->last_power) {
      int pg = hp_store_find_act(&a->store, (uint8_t)(a->last_activity + 1));
      if (pg >= 0) a->cur = pg;
    }
    show_cur(a);
  } else if (raw[1] == a->cur || !on_stored_page(a)) { /* the visible page was replaced, or the awaited page arrived */
    show_cur(a);
  }
}

/* state_update: the highlight always; the page only on a change: a running activity shows its `act` page, power off
 * shows Home. */
static void state_v2(app_t *a, uint8_t activity, bool power) {
  bool changed = !a->state_known || a->last_activity != activity || a->last_power != power;
  a->state_known = true;
  ra_ui_set_activity(&a->ui, activity, power);
  if (!a->v2 || !changed || !hp_store_complete(&a->store)) return;
  if (power) {
    int pg = hp_store_find_act(&a->store, (uint8_t)(activity + 1));
    if (pg >= 0) goto_page(a, pg);
  } else {
    go_home(a);
  }
}

static void handle_link_event(app_t *a, const app_link_event_t *ev, uint64_t now) {
  switch (ev->kind) {
  case APP_LINK_EV_PAGE_PUSH: {
    heard(a, now);
    if (ev->len >= 1 && ev->data[0] == 2) {
      push_v2(a, ev->data, ev->len);
      break;
    }
    hp_page_t p;
    int r = hp_page_decode(ev->data, ev->len, &p);
    if (r < 0) {
      ra_logf(&a->sys, "page_push rejected (%d)", r);
    } else {
      a->v2 = false; /* a version 1 page: the old single-page behaviour */
      a->saved = p;
      a->have_saved = true;
      ra_display_note(&a->display, RA_WHY_PAGE, 0);
      ra_ui_show_page(&a->ui, &p);
    }
    break;
  }
  case APP_LINK_EV_TILE_UPDATE:
  case APP_LINK_EV_TILE_STATE:
    heard(a, now);
    ra_display_note(&a->display, RA_WHY_STATUS, 0);
    if (a->v2) {
      uint8_t st = ev->kind == APP_LINK_EV_TILE_STATE ? ev->state : (uint8_t)HP_TILE_VALUE;
      if (hp_store_tile(&a->store, ev->page, ev->tile, st, ev->text, strlen(ev->text)) < 0 ||
          (on_stored_page(a) && ra_ui_tile_state(&a->ui, ev->page, ev->tile, st, ev->text, strlen(ev->text)) < 0 &&
           ev->page == a->cur))
        ra_logf(&a->sys, "tile %u/%u ignored", ev->page, ev->tile);
      break;
    }
    if (a->have_saved && a->saved.page_id == ev->page) (void)hp_page_set_tile_text(&a->saved, ev->tile, ev->text, strlen(ev->text));
    if (ra_ui_tile_text(&a->ui, ev->page, ev->tile, ev->text, strlen(ev->text)) < 0)
      ra_logf(&a->sys, "tile_update %u/%u ignored", ev->page, ev->tile);
    break;
  case APP_LINK_EV_STATE_UPDATE:
    heard(a, now);
    a->state_updates++;
    state_v2(a, ev->activity, ev->power);
    a->last_activity = ev->activity;
    a->last_power = ev->power;
    if (!a->have_saved && !a->v2 && !a->ui.fallback && a->ui.page.page_id == RA_UI_PAGE_BOOT) ra_ui_show_receiving(&a->ui);
    break;
  case APP_LINK_EV_ALIVE:
    heard(a, now);
    if (a->ui.fallback) ra_display_note(&a->display, RA_WHY_PAGE, 0);
    if (a->ui.fallback) { /* the hub is back: the last real page, not a blank wait for the next push */
      if (a->v2) show_cur(a);
      else if (a->have_saved) ra_ui_show_page(&a->ui, &a->saved);
      else ra_ui_show_receiving(&a->ui);
    } else if (!a->have_saved && !a->v2 && a->ui.page.page_id == RA_UI_PAGE_BOOT) {
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
  a->input_ms = now;
  if (w > 0)
    for (int i = 0; i < 2; i++)
      if (ready[i]) ra_input_read(&a->input, i, now);
  ra_input_tick(&a->input, now);
  if (w > 0 || a->ntx) {
    lv_refr_now(a->display.disp); /* touch / key feedback: pixels now, not at the next refresh timer */
    send_pending(a);              /* then the radio */
  }
  ra_power_tick(&a->power, now);
  if (a->power.batt_polls != a->last_batt_polls) {
    a->last_batt_polls = a->power.batt_polls;
    ra_display_note(&a->display, RA_WHY_STATUS, 0);
    ra_ui_set_battery(&a->ui, a->power.soc, a->power.external_power);
    /* Low-battery toast with hysteresis: queued at <= 10 % off the charger, once; re-armed only at >= 13 % or on
     * external power. It is shown when the screen is on (see below), not while asleep. */
    if (a->power.external_power || a->power.soc >= RA_LOWBATT_REARM_PCT) {
      a->lowbatt_disarmed = false;
      if (a->power.external_power) a->lowbatt_pending = false;
    } else if (a->power.soc <= RA_LOWBATT_PCT && !a->lowbatt_disarmed) {
      a->lowbatt_disarmed = true;
      a->lowbatt_pending = true;
    }
  }
  bool awake = a->power.state != RA_PW_OFF;
  if (a->link.set_awake && (!a->awake_known || awake != a->last_awake)) a->link.set_awake(a->link.ctx, awake);
  if (a->awake_known && awake && !a->last_awake) ra_display_note(&a->display, RA_WHY_WAKE, 0);
  a->awake_known = true;
  a->last_awake = awake;
  app_link_event_t ev;
  for (int i = 0; i < LINK_EVENTS_PER_STEP && a->link.poll(a->link.ctx, now, &ev) > 0; i++) handle_link_event(a, &ev, now);
  lv_timer_ready(lv_display_get_refr_timer(a->display.disp)); /* a page, tile or status change: draw it now, not at the next 33 ms tick */
  lv_timer_handler(); /* render what changed */
  if (a->lowbatt_pending && a->power.state != RA_PW_OFF) { /* the screen is on and its frame is drawn: show it now */
    char b[24];
    snprintf(b, sizeof b, "Battery %d%%", a->power.soc);
    ra_ui_toast(&a->ui, b, "Place on the charger");
    a->lowbatt_pending = false;
  }
  if (a->ui.oom != a->oom_logged) {
    a->oom_logged = a->ui.oom;
    ra_logf(&a->sys, "ui: out of memory building a screen; showing the error state (%u so far)", a->ui.oom);
  }
  if (!a->healthy_written && a->input_open && a->display.frames >= 1 && ra_write_bootcount(&a->sys) == 0)
    a->healthy_written = true;
}

void app_destroy(app_t *a) {
  ra_input_close(&a->input);
  ra_display_destroy(&a->display);
  lv_deinit();
  g_tick_sys = NULL;
}
