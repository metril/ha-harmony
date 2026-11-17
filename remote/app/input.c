#include "remote/app/input.h"

#include <errno.h>
#include <string.h>

#include "proto/gen/harmony_proto.h"

int ra_input_open(ra_input_t *in, ra_sys_t *sys, const ra_input_sink_t *sink) {
  memset(in, 0, sizeof *in);
  in->sys = sys;
  in->sink = *sink;
  in->fd[0] = in->fd[1] = -1;
  in->fd[0] = ra_open_input(sys, RA_PATH_KEYS);
  if (in->fd[0] >= 0) in->fd[1] = ra_open_input(sys, RA_PATH_TOUCH);
  if (in->fd[0] < 0 || in->fd[1] < 0) {
    ra_input_close(in);
    return -1;
  }
  return 0;
}

void ra_input_close(ra_input_t *in) {
  for (int i = 0; i < 2; i++)
    if (in->fd[i] >= 0) ra_close(in->sys, in->fd[i]);
  in->fd[0] = in->fd[1] = -1;
}

static ra_held_t *find(ra_input_t *in, uint16_t evdev) {
  for (int i = 0; i < RA_MAX_HELD; i++)
    if (in->held[i].code && in->held[i].evdev == evdev) return &in->held[i];
  return NULL;
}

static void key_event(ra_input_t *in, const struct input_event *ev, uint64_t now) {
  if (ev->value == 2) return; /* kernel autorepeat: we generate our own holds */
  if (ev->value == 1) {
    uint8_t code = hp_key_from_evdev(ev->code);
    if (code == HP_KEY_NONE || find(in, ev->code)) return; /* unknown key: dropped */
    ra_held_t *h = NULL; /* free slot: code == 0 is never a real key (ids start at 1) */
    for (int i = 0; i < RA_MAX_HELD && !h; i++)
      if (!in->held[i].code) h = &in->held[i];
    if (!h) return;
    bool pass = in->sink.gate(in->sink.ctx, now);
    *h = (ra_held_t){.evdev = ev->code, .code = code, .down_ms = now, .next_hold_ms = now + RA_HOLD_DELAY_MS,
                     .suppressed = !pass};
    if (pass) in->sink.emit(in->sink.ctx, code, HP_KEY_ACTION_PRESS);
  } else if (ev->value == 0) {
    ra_held_t *h = find(in, ev->code);
    if (!h) return;
    if (!h->suppressed) {
      in->sink.activity(in->sink.ctx, now);
      in->sink.emit(in->sink.ctx, h->code, HP_KEY_ACTION_RELEASE);
    }
    memset(h, 0, sizeof *h);
  }
}

static void touch_release(ra_input_t *in, uint64_t now) {
  in->touching = false;
  if (in->touch_suppressed) {
    in->touch_suppressed = false;
    return;
  }
  in->sink.activity(in->sink.ctx, now);
  const hp_page_t *p = in->sink.page(in->sink.ctx);
  const hp_widget_t *w = p ? hp_page_hit(p, in->tx, in->ty) : NULL;
  if (!w || w->kind == HP_WIDGET_LABEL) return;
  uint8_t code = (uint8_t)(HP_SOFT_KEY_BASE + w->id);
  in->sink.emit(in->sink.ctx, code, HP_KEY_ACTION_PRESS);
  in->sink.emit(in->sink.ctx, code, HP_KEY_ACTION_RELEASE);
}

void ra_input_feed(ra_input_t *in, const struct input_event *ev, uint64_t now) {
  if (ev->type == EV_ABS) {
    if (ev->code == ABS_X) in->tx = ev->value;
    else if (ev->code == ABS_Y) in->ty = ev->value;
  } else if (ev->type == EV_KEY) {
    if (ev->code == BTN_TOUCH) {
      if (ev->value == 1 && !in->touching) {
        in->touching = true;
        in->touch_suppressed = !in->sink.gate(in->sink.ctx, now);
      } else if (ev->value == 0 && in->touching) {
        touch_release(in, now);
      }
    } else {
      key_event(in, ev, now);
    }
  }
}

void ra_input_read(ra_input_t *in, int idx, uint64_t now) {
  struct input_event evs[16];
  ssize_t n = ra_read(in->sys, in->fd[idx], evs, sizeof evs);
  if (n <= 0) {
    if (n < 0 && errno != EINTR && errno != EAGAIN) ra_logf(in->sys, "input read failed: %s", strerror(errno));
    return;
  }
  for (size_t i = 0; i < (size_t)n / sizeof evs[0]; i++) ra_input_feed(in, &evs[i], now);
}

void ra_input_tick(ra_input_t *in, uint64_t now) {
  for (int i = 0; i < RA_MAX_HELD; i++) {
    ra_held_t *h = &in->held[i];
    if (!h->code || h->suppressed || now < h->next_hold_ms) continue;
    in->sink.activity(in->sink.ctx, now); /* a held key is not idle */
    in->sink.emit(in->sink.ctx, h->code, HP_KEY_ACTION_HOLD);
    while (h->next_hold_ms <= now) h->next_hold_ms += RA_HOLD_REPEAT_MS;
  }
}

uint32_t ra_input_next_ms(const ra_input_t *in, uint64_t now) {
  uint64_t best = UINT32_MAX;
  for (int i = 0; i < RA_MAX_HELD; i++) {
    const ra_held_t *h = &in->held[i];
    if (!h->code || h->suppressed) continue;
    uint64_t d = h->next_hold_ms > now ? h->next_hold_ms - now : 0;
    if (d < best) best = d;
  }
  return (uint32_t)best;
}

bool ra_input_any_held(const ra_input_t *in) {
  for (int i = 0; i < RA_MAX_HELD; i++)
    if (in->held[i].code) return true;
  return in->touching;
}
