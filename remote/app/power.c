#include "remote/app/power.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_node(ra_power_t *p, const char *path, int v) {
  char s[8];
  snprintf(s, sizeof s, "%d", v);
  if (ra_sysfs_write(p->sys, path, s) < 0) {
    if (!p->write_failed) ra_logf(p->sys, "write %s failed", path); /* once per failure streak */
    p->write_failed = true;
  } else {
    p->write_failed = false;
  }
}

static void set_led(ra_power_t *p, int v) {
  if (p->led == v) return;
  p->led = v;
  write_node(p, RA_PATH_LED, v);
}

static int target_for(ra_pw_state_t s) { return s == RA_PW_ACTIVE ? RA_BL_NORMAL : s == RA_PW_DIM ? RA_BL_DIM : RA_BL_OFF; }

static void set_state(ra_power_t *p, ra_pw_state_t s, uint64_t now) {
  if (s == RA_PW_OFF && p->state != RA_PW_OFF) p->off_since_ms = now;
  p->state = s;
  p->bl_target = target_for(s);
  set_led(p, s == RA_PW_ACTIVE ? RA_LED_ON : RA_LED_OFF);
}

void ra_power_init(ra_power_t *p, ra_sys_t *sys, uint64_t now) {
  memset(p, 0, sizeof *p);
  p->sys = sys;
  p->soc = -1;
  p->last_input_ms = p->last_step_ms = p->next_batt_ms = now;
  p->state = RA_PW_ACTIVE;
  p->bl = p->bl_target = RA_BL_NORMAL;
  p->led = RA_LED_ON;
  write_node(p, RA_PATH_BACKLIGHT, p->bl);
  write_node(p, RA_PATH_LED, p->led);
}

bool ra_power_input(ra_power_t *p, uint64_t now, bool touch) {
  ra_pw_state_t was = p->state;
  p->last_input_ms = now;
  if (was != RA_PW_ACTIVE) set_state(p, RA_PW_ACTIVE, now);
  /* A dimmed screen is readable: everything acts. Off: hard keys act too, only a touch is swallowed. */
  bool swallow = was == RA_PW_OFF && touch;
  if (swallow && (!p->swallow_logged || now - p->swallow_log_ms >= RA_SWALLOW_LOG_MS)) {
    p->swallow_logged = true;
    p->swallow_log_ms = now;
    ra_logf(p->sys, "power: touch on dark screen woke it (not sent)");
  }
  return !swallow;
}

bool ra_power_lift(ra_power_t *p, uint64_t now) {
  if (p->state == RA_PW_ACTIVE || p->poweroff_requested) return false;
  if (p->state == RA_PW_OFF && now - p->off_since_ms < RA_LIFT_SETTLE_MS) return false;
  p->last_input_ms = now;
  p->lifts++;
  set_state(p, RA_PW_ACTIVE, now);
  return true;
}

void ra_power_activity(ra_power_t *p, uint64_t now) {
  if (p->state == RA_PW_ACTIVE) p->last_input_ms = now;
}

/* First whitespace-delimited token as a finite double; false on empty, garbage or trailing junk. */
static bool first_number(const char *s, double *out) {
  while (isspace((unsigned char)*s)) s++;
  char *end;
  double v = strtod(s, &end);
  if (end == s || !isfinite(v) || (*end && !isspace((unsigned char)*end))) return false;
  *out = v;
  return true;
}

int ra_battery_should_poweroff(const char *soc, const char *powersource, const char *battcharge) {
  double s, c;
  if (!first_number(soc, &s) || s < 0.0 || s > 100.0) return 0;
  if (!first_number(battcharge, &c)) return 0;
  size_t n = strlen(powersource);
  while (n && isspace((unsigned char)powersource[n - 1])) n--;
  if (n != 7 || memcmp(powersource, "battery", 7) != 0) return 0;
  return s <= RA_BATT_SOC_MAX && c <= 0.0;
}

int ra_battery_percent(ra_sys_t *s) {
  char b[32];
  double d;
  if (ra_sysfs_read(s, RA_PATH_SOC, b, sizeof b) < 0 || !first_number(b, &d) || d < 0.0 || d > 100.0) return -1;
  return (int)(d + 0.5);
}

static void battery_poll(ra_power_t *p) {
  char soc[32], src[32], chg[48];
  /* All three in this one poll: a reading is never reused from an earlier poll. */
  if (ra_sysfs_read(p->sys, RA_PATH_SOC, soc, sizeof soc) < 0 ||
      ra_sysfs_read(p->sys, RA_PATH_POWERSRC, src, sizeof src) < 0 ||
      ra_sysfs_read(p->sys, RA_PATH_BATTCHARGE, chg, sizeof chg) < 0) {
    ra_logf(p->sys, "battery poll: read failed, not powering off");
    return;
  }
  double d;
  if (!first_number(soc, &d)) {
    ra_logf(p->sys, "battery poll: unparsable soc, not powering off");
    return;
  }
  if (d >= 0.0 && d <= 100.0) {
    size_t n = strlen(src);
    while (n && isspace((unsigned char)src[n - 1])) n--;
    p->soc = (int)(d + 0.5);
    p->external_power = !(n == 7 && memcmp(src, "battery", 7) == 0);
    p->batt_polls++;
  }
  if (!ra_battery_should_poweroff(soc, src, chg)) return;
  ra_logf(p->sys, "battery %.1f%% on battery, not charging: powering off", d);
  if (ra_poweroff(p->sys) == 0) p->poweroff_requested = true;
  else ra_logf(p->sys, "poweroff failed, will retry");
}

bool ra_power_suspend_due(ra_power_t *p, uint64_t now) {
  if (p->suspend_mode == RA_SUSPEND_NEVER || p->state != RA_PW_OFF || p->bl != 0 || p->poweroff_requested) return false;
  if (now < p->off_since_ms + RA_SUSPEND_AFTER_MS || now < p->suspend_hold_ms) return false;
  if (p->suspend_mode == RA_SUSPEND_BATTERY) {
    char src[32];
    size_t n = ra_sysfs_read(p->sys, RA_PATH_POWERSRC, src, sizeof src) < 0 ? 0 : strlen(src);
    while (n && isspace((unsigned char)src[n - 1])) n--;
    if (n != 7 || memcmp(src, "battery", 7) != 0) { /* powered, or unreadable: never suspend on a guess */
      p->suspend_hold_ms = now + RA_SUSPEND_RECHECK_MS;
      return false;
    }
  }
  return true;
}

void ra_power_suspend_failed(ra_power_t *p, uint64_t now) { p->suspend_hold_ms = now + RA_SUSPEND_RETRY_MS; }

void ra_power_resumed(ra_power_t *p, uint64_t now, uint64_t gap_ms) {
  p->suspends++;
  p->suspended_ms += gap_ms;
  p->last_input_ms = p->last_step_ms = p->next_batt_ms = now;
  p->suspend_hold_ms = now + RA_SUSPEND_WAKE_HOLD_MS;
  set_state(p, RA_PW_ACTIVE, now);
}

uint64_t ra_power_gap_ms(uint64_t wall0, uint64_t mono0, uint64_t wall1, uint64_t mono1) {
  if (wall1 < wall0 || mono1 < mono0) return 0;
  uint64_t w = wall1 - wall0, m = mono1 - mono0;
  uint64_t g = w > m ? w - m : 0;
  return g > RA_SUSPEND_GAP_MAX_MS ? 0 : g;
}

void ra_power_tick(ra_power_t *p, uint64_t now) {
  if (p->state == RA_PW_ACTIVE) {
    uint64_t idle = now - p->last_input_ms;
    if (idle >= RA_OFF_AFTER_MS) set_state(p, RA_PW_OFF, now);
    else if (idle >= RA_DIM_AFTER_MS) set_state(p, RA_PW_DIM, now);
  } else if (p->state == RA_PW_DIM && now - p->last_input_ms >= RA_OFF_AFTER_MS) {
    set_state(p, RA_PW_OFF, now);
  }
  if (p->bl != p->bl_target && now - p->last_step_ms >= RA_BL_STEP_MS) {
    p->bl += p->bl < p->bl_target ? 1 : -1;
    p->last_step_ms = now;
    write_node(p, RA_PATH_BACKLIGHT, p->bl);
  }
  if (!p->poweroff_requested && now >= p->next_batt_ms) {
    p->next_batt_ms = now + RA_BATT_POLL_MS;
    battery_poll(p);
  }
}

uint32_t ra_power_next_ms(const ra_power_t *p, uint64_t now) {
  uint64_t best = p->poweroff_requested ? UINT32_MAX : (p->next_batt_ms > now ? p->next_batt_ms - now : 0);
  if (p->bl != p->bl_target) {
    uint64_t d = p->last_step_ms + RA_BL_STEP_MS > now ? p->last_step_ms + RA_BL_STEP_MS - now : 0;
    if (d < best) best = d;
  }
  if (p->state == RA_PW_ACTIVE) {
    uint64_t at = p->last_input_ms + RA_DIM_AFTER_MS, at2 = p->last_input_ms + RA_OFF_AFTER_MS;
    uint64_t d = (at > now ? at - now : 0);
    if (at <= now) d = at2 > now ? at2 - now : 0;
    if (d < best) best = d;
  } else if (p->state == RA_PW_DIM) {
    uint64_t at = p->last_input_ms + RA_OFF_AFTER_MS;
    uint64_t d = at > now ? at - now : 0;
    if (d < best) best = d;
  }
  if (p->suspend_mode != RA_SUSPEND_NEVER && p->state == RA_PW_OFF && !p->poweroff_requested) {
    uint64_t at = p->off_since_ms + RA_SUSPEND_AFTER_MS;
    if (p->suspend_hold_ms > at) at = p->suspend_hold_ms;
    uint64_t d = at > now ? at - now : 0;
    if (d < best) best = d;
  }
  return best > UINT32_MAX ? UINT32_MAX : (uint32_t)best;
}
