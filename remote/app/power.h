#ifndef HP_REMOTE_POWER_H
#define HP_REMOTE_POWER_H

/* Backlight + key LEDs, the idle state machine and the battery guard. Suspend to RAM
 * (/sys/power/state, the CPU clock is never touched) after the screen has been off a while, on battery. */
#include <stdbool.h>
#include <stdint.h>

#include "remote/app/sys.h"

#define RA_BL_NORMAL 26 /* stock maximum */
#define RA_BL_DIM 5
#define RA_BL_OFF 0
#define RA_BL_STEP_MS 50 /* one brightness step per interval, as stock fades */
#define RA_LED_ON 42
#define RA_LED_OFF 0
#define RA_DIM_AFTER_MS 10000 /* idle time to dim */
#define RA_OFF_AFTER_MS 30000 /* idle time to switch the backlight off */
#define RA_BATT_POLL_MS 60000
#define RA_BATT_SOC_MAX 3.0 /* percent: at or below this, on battery and not charging, power off */

#define RA_SUSPEND_AFTER_MS 10000   /* screen off (fade done) this long, then suspend to RAM */
#define RA_SUSPEND_RECHECK_MS 30000 /* not eligible (powered): look again after this */
#define RA_SUSPEND_RETRY_MS 60000   /* the kernel refused: wait before trying again */
#define RA_SUSPEND_WAKE_HOLD_MS 8000 /* after a resume: stay up to re-link and report before sleeping again */
#define RA_SUSPEND_GAP_MAX_MS (14ULL * 24 * 3600 * 1000) /* a longer measured gap is a clock jump, not a sleep */

/* NEVER: no suspend (the default: opt-in). BATTERY: only when the gauge's powersource is exactly "battery" (never in the
 * cradle or on USB). ALWAYS: also when powered (bench only: USB soak runs). */
typedef enum { RA_SUSPEND_NEVER, RA_SUSPEND_BATTERY, RA_SUSPEND_ALWAYS } ra_suspend_mode_t;

typedef enum { RA_PW_ACTIVE, RA_PW_DIM, RA_PW_OFF } ra_pw_state_t;

typedef struct {
  ra_sys_t *sys;
  ra_pw_state_t state;
  uint64_t last_input_ms, last_step_ms, next_batt_ms;
  int bl, bl_target, led;
  bool write_failed, poweroff_requested;
  ra_suspend_mode_t suspend_mode;
  uint64_t off_since_ms, suspend_hold_ms; /* when the screen went off; no suspend before this time */
  uint64_t suspended_ms; /* total time asleep, from the wall clock (CLOCK_MONOTONIC stops in suspend) */
  unsigned suspends;
  /* Last good gauge poll, for the status bar: soc percent (-1 until the first poll), whether a charger
   * is attached (powersource is not "battery"), and the number of good polls. */
  int soc;
  bool external_power;
  unsigned batt_polls;
} ra_power_t;

/* Backlight to normal and LEDs on immediately; first battery poll on the first tick. */
void ra_power_init(ra_power_t *p, ra_sys_t *sys, uint64_t now);
/* A press / touch-down arrived. True: the screen was active and the event goes on. False: it woke
 * the screen (from dim or off) and must be consumed. */
bool ra_power_input(ra_power_t *p, uint64_t now);
/* Other accepted input: restart the idle timer (never wakes). */
void ra_power_activity(ra_power_t *p, uint64_t now);
void ra_power_tick(ra_power_t *p, uint64_t now);
uint32_t ra_power_next_ms(const ra_power_t *p, uint64_t now);

/* True when the app should now blank the panel and call ra_suspend(): the screen has been off RA_SUSPEND_AFTER_MS
 * (backlight at 0), the hold has passed, and the mode allows the current power source (re-read here). */
bool ra_power_suspend_due(ra_power_t *p, uint64_t now);
/* Suspend returned an error: back off RA_SUSPEND_RETRY_MS. */
void ra_power_suspend_failed(ra_power_t *p, uint64_t now);
/* The kernel resumed after gap_ms asleep (wall clock minus monotonic, see ra_power_gap_ms): the screen wakes, the idle
 * timer restarts, the next tick polls the battery (the guard runs at once) and sleep is held RA_SUSPEND_WAKE_HOLD_MS. */
void ra_power_resumed(ra_power_t *p, uint64_t now, uint64_t gap_ms);
/* Time slept = wall delta minus monotonic delta, floored at 0; 0 when implausible (> RA_SUSPEND_GAP_MAX_MS). */
uint64_t ra_power_gap_ms(uint64_t wall0, uint64_t mono0, uint64_t wall1, uint64_t mono1);

/* The battery guard's decision, pure: 1 only for a parsed soc <= 3.0, powersource exactly "battery"
 * and a battcharge current <= 0. Any parse failure is 0. */
int ra_battery_should_poweroff(const char *soc, const char *powersource, const char *battcharge);

/* The gauge's state of charge as a rounded percent 0..100 (one RA_PATH_SOC read, same parser as the
 * guard), or -1 when the read fails or is unparsable / out of range. */
int ra_battery_percent(ra_sys_t *s);

#endif
