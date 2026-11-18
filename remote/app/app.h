#ifndef HP_REMOTE_APP_H
#define HP_REMOTE_APP_H

/* The remote app: single-threaded main loop gluing display, ui, input, power and the RF link. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remote/app/display.h"
#include "remote/app/input.h"
#include "remote/app/power.h"
#include "remote/app/sys.h"
#include "remote/app/ui.h"
#include "remote/page/store.h"

typedef enum {
  APP_LINK_EV_NONE = 0,
  APP_LINK_EV_PAGE_PUSH,   /* page, data[0..len) */
  APP_LINK_EV_TILE_UPDATE, /* page, tile, text */
  APP_LINK_EV_TILE_STATE,  /* page, tile, state, text (hp_tile_state_t) */
  APP_LINK_EV_STATE_UPDATE,/* activity, power */
  APP_LINK_EV_FAILED,      /* a send failed after all retries (fatal: the link cannot work at all) */
  APP_LINK_EV_ALIVE        /* any frame arrived from the hub */
} app_link_event_kind_t;

typedef struct {
  app_link_event_kind_t kind;
  uint8_t page, tile, activity, state;
  bool power, fatal; /* fatal: FAILED that shows the fallback at once */
  uint8_t data[HP_PAGE_MAX_BYTES];
  size_t len;
  char text[HP_PAGE_MAX_TEXT + 1];
} app_link_event_t;

/* The RF side (remote/app/rf_link.h supplies the real one); the default is a no-op. */
typedef struct {
  void *ctx;
  /* Queue a key event; 0 ok. Delivery failure after retries comes back later as APP_LINK_EV_FAILED. */
  int (*send_key)(void *ctx, uint8_t code, uint8_t action);
  /* Next pending event into *ev (returns 1), or 0. Called repeatedly each loop iteration. */
  int (*poll)(void *ctx, uint64_t now, app_link_event_t *ev);
  /* Optional (NULL = none). An fd that is readable when poll() has work: the loop select()s on it. */
  int (*get_fd)(void *ctx);
  /* Optional. Milliseconds until the link next needs a poll (retry deadline, heartbeat), 0 = now,
   * UINT32_MAX = nothing scheduled. Bounds the loop's wait. */
  uint32_t (*next_ms)(void *ctx, uint64_t now);
  /* Optional. The screen is awake (not backlight-off): the link polls the hub only then. */
  void (*set_awake)(void *ctx, bool awake);
} app_link_t;

extern const app_link_t app_link_null;

typedef struct {
  ra_sys_t sys;
  app_link_t link;
  ra_display_t display;
  ra_ui_t ui;
  ra_input_t input;
  ra_power_t power;
  bool input_open, healthy_written;
  unsigned state_updates; /* count of state_update events seen (content unused until the UI shows it) */
  uint8_t last_activity;
  bool last_power;
  /* Fallback policy: >= APP_FAIL_COUNT consecutive FAILED events AND no hub frame for APP_FAIL_SILENCE_MS. */
  unsigned fail_run;
  uint64_t last_rx_ms;
  hp_page_t saved; /* version 1: the last real page pushed, restored when the hub is heard again */
  bool have_saved, awake_known, last_awake;
  /* Version 2: the page store (raw wire bytes, ~9.4 KiB) and navigation. `v2` once a v2 page has been stored; `cur`
   * is the page id on screen or being waited for (-1 none). */
  hp_store_t store;
  bool v2, state_known;
  bool home_req;          /* a tap on the error screen: go to the start page after the input is handled */
  bool partial;           /* the set is incomplete and the best stored page is shown (timeout, or the user pressed Home) */
  uint64_t last_page_ms;  /* when the last new v2 page arrived */
  int cur;
  uint8_t key_up, key_down, key_exit, key_home, key_fav; /* logical ids of the hard keys the app handles itself */
  uint8_t swallowed;                                     /* bit 0..2: Up, Down, Exit were consumed at press: their holds / release too */
  unsigned last_batt_polls; /* power.batt_polls already shown in the status bar */
  bool lowbatt_disarmed;    /* the low-battery toast was queued for this dip (re-arms at >= 13 % or on a charger) */
  bool lowbatt_pending;     /* queued, waiting for the screen to be on */
  struct { uint8_t code, action; } tx[16]; /* key events queued while handling input; sent after the redraw */
  unsigned ntx;
  uint64_t input_ms;        /* when the current batch of input events was read (for --ui-debug) */
  unsigned oom_logged;      /* ui.oom already reported in the log */
} app_t;

/* 0 ok; <0 refused to start (bad panel, cannot open the framebuffer or input). One app per process. */
int app_init(app_t *a, const ra_ops_t *ops, const app_link_t *link, const char *bootcount_path);
/* One loop iteration: wait for input up to max_wait_ms (less when a timer is due), then process
 * input, holds, power, link events and LVGL timers. */
void app_step(app_t *a, unsigned max_wait_ms);

/* An incomplete page set shows the best stored page this long after its last page arrived. */
#define APP_PARTIAL_MS 10000
#define APP_FAIL_COUNT 3
#define APP_FAIL_SILENCE_MS 5000

/* Cap on the wait when the link has an fd to wake on (it is woken by a frame, and by the link's
 * own deadline): idle wakeups are rare. Without an fd the cap stays 100 ms (polling). */
#define APP_LINK_IDLE_WAIT_MS 10000
void app_destroy(app_t *a);

#endif
