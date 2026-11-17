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

typedef enum {
  APP_LINK_EV_NONE = 0,
  APP_LINK_EV_PAGE_PUSH,   /* page, data[0..len) */
  APP_LINK_EV_TILE_UPDATE, /* page, tile, text */
  APP_LINK_EV_STATE_UPDATE,/* activity, power */
  APP_LINK_EV_FAILED       /* a send failed after all retries */
} app_link_event_kind_t;

typedef struct {
  app_link_event_kind_t kind;
  uint8_t page, tile, activity;
  bool power;
  uint8_t data[HP_PAGE_MAX_BYTES];
  size_t len;
  char text[HP_PAGE_MAX_TEXT + 1];
} app_link_event_t;

/* The RF side. Task 3 supplies the real one; the default is a no-op. */
typedef struct {
  void *ctx;
  /* Queue a key event; 0 ok. Delivery failure after retries comes back later as APP_LINK_EV_FAILED. */
  int (*send_key)(void *ctx, uint8_t code, uint8_t action);
  /* Next pending event into *ev (returns 1), or 0. Called repeatedly each loop iteration. */
  int (*poll)(void *ctx, uint64_t now, app_link_event_t *ev);
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
} app_t;

/* 0 ok; <0 refused to start (bad panel, cannot open the framebuffer or input). One app per process. */
int app_init(app_t *a, const ra_ops_t *ops, const app_link_t *link, const char *bootcount_path);
/* One loop iteration: wait for input up to max_wait_ms (less when a timer is due), then process
 * input, holds, power, link events and LVGL timers. */
void app_step(app_t *a, unsigned max_wait_ms);
void app_destroy(app_t *a);

#endif
