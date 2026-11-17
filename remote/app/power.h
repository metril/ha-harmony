#ifndef HP_REMOTE_POWER_H
#define HP_REMOTE_POWER_H

/* Backlight + key LEDs, the idle state machine and the battery guard. No MSP430 suspend. */
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

typedef enum { RA_PW_ACTIVE, RA_PW_DIM, RA_PW_OFF } ra_pw_state_t;

typedef struct {
  ra_sys_t *sys;
  ra_pw_state_t state;
  uint64_t last_input_ms, last_step_ms, next_batt_ms;
  int bl, bl_target, led;
  bool write_failed, poweroff_requested;
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

/* The battery guard's decision, pure: 1 only for a parsed soc <= 3.0, powersource exactly "battery"
 * and a battcharge current <= 0. Any parse failure is 0. */
int ra_battery_should_poweroff(const char *soc, const char *powersource, const char *battcharge);

#endif
