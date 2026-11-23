#ifndef HP_REMOTE_INPUT_H
#define HP_REMOTE_INPUT_H

/* evdev reader: hard keys (event0), touch + Home/Favorites strip (event2). Read-only, never grabs.
 * Emits logical key codes with proto's key actions (press 0, release 1, hold 2). */
#include <stdbool.h>
#include <stdint.h>

#include <linux/input.h>

#include "remote/app/sys.h"
#include "remote/page/page.h"

#define RA_HOLD_DELAY_MS 500  /* first hold after the key has been down this long */
#define RA_HOLD_REPEAT_MS 200 /* then one hold per interval while still down */
#define RA_MAX_HELD 8

typedef struct {
  void *ctx;
  void (*emit)(void *ctx, uint8_t code, uint8_t action);
  /* A press (key down / touch down; touch says which) is about to be acted on. Return false when it only woke the
   * screen: the event, and its release, are then consumed. */
  bool (*gate)(void *ctx, uint64_t now, bool touch);
  void (*activity)(void *ctx, uint64_t now); /* any other accepted input: restart the idle timer */
  const hp_page_t *(*page)(void *ctx);       /* page used to hit-test taps */
  /* Optional (NULL = none). Finger down / moved at pixel (x,y): down true; lifted: down false.
   * Visual feedback only; never called for a touch the gate consumed. */
  void (*touch)(void *ctx, int x, int y, bool down);
  /* Optional. A tap (release) on a button / tile: return true when the app handled it locally (a nav widget), so
   * no key_event is sent. */
  bool (*widget)(void *ctx, const hp_widget_t *w);
  /* Optional. A tap (release) in the status bar (y < HP_PAGE_BAR_H). */
  void (*bar)(void *ctx, int x, int y);
} ra_input_sink_t;

typedef struct {
  uint16_t evdev;
  uint8_t code;
  uint64_t down_ms, next_hold_ms;
  bool suppressed;
} ra_held_t;

typedef struct {
  ra_sys_t *sys;
  ra_input_sink_t sink;
  int fd[2]; /* event0, event2 */
  ra_held_t held[RA_MAX_HELD];
  int tx, ty;
  bool touching, touch_suppressed;
} ra_input_t;

/* 0 ok, -1 an input node failed to open (the app does not run without input). */
int ra_input_open(ra_input_t *in, ra_sys_t *sys, const ra_input_sink_t *sink);
void ra_input_close(ra_input_t *in);
/* Read what fd[idx] has (one read of up to 16 events) and process it. */
void ra_input_read(ra_input_t *in, int idx, uint64_t now);
void ra_input_feed(ra_input_t *in, const struct input_event *ev, uint64_t now);
void ra_input_tick(ra_input_t *in, uint64_t now);
/* Milliseconds until the next hold is due, or UINT32_MAX when no key is down. */
uint32_t ra_input_next_ms(const ra_input_t *in, uint64_t now);
bool ra_input_any_held(const ra_input_t *in);

#endif
