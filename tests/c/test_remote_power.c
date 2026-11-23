/* Idle state machine, backlight fade, key LEDs and the battery guard, with an injected clock. */
#include "remote_fake.h"

#define SOC RA_PATH_SOC
#define SRC RA_PATH_POWERSRC
#define CHG RA_PATH_BATTCHARGE

static fk_t fk;
static ra_sys_t sys;
static ra_power_t pw;

static void begin(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  ra_sys_init(&sys, &ops, NULL);
  ra_power_init(&pw, &sys, fk.now);
}

/* Advance in 50 ms steps, ticking each time. */
static void run_to(uint64_t t) {
  while (fk.now < t) {
    fk.now += 50;
    ra_power_tick(&pw, fk.now);
  }
}

static void test_constants(void) {
  CT_EQ(RA_BL_NORMAL, 26);
  CT_EQ(RA_BL_DIM, 5);
  CT_EQ(RA_BL_OFF, 0);
  CT_EQ(RA_LED_ON, 42);
  CT_EQ(RA_BL_STEP_MS, 50);
  CT_EQ(RA_DIM_AFTER_MS, 10000);
  CT_EQ(RA_OFF_AFTER_MS, 30000);
  CT_EQ(RA_BATT_POLL_MS, 60000);
}

static void test_idle_machine(void) {
  begin();
  CT_EQ(fk.bl_w[0], 26);
  CT_EQ(fk.led_w[0], 42);
  uint64_t t0 = fk.now;
  run_to(t0 + 9950);
  CT_EQ(pw.state, RA_PW_ACTIVE);
  CT_EQ(pw.bl, 26);
  run_to(t0 + 10000);
  CT_EQ(pw.state, RA_PW_DIM);
  CT_EQ(fk.led_w[fk.nled - 1], 0); /* key LEDs go off with the dim */
  /* one step per 50 ms, from 26 down to 5 */
  int before = fk.nbl;
  run_to(t0 + 10000 + 50 * 21);
  CT_EQ(fk.nbl - before, 20); /* the first of the 21 steps came with the dim tick itself */
  CT_EQ(pw.bl, 5);
  for (int i = 1; i < fk.nbl; i++) CT_CHECK(abs(fk.bl_w[i] - fk.bl_w[i - 1]) == 1); /* strictly stepwise */
  run_to(t0 + 29950);
  CT_EQ(pw.state, RA_PW_DIM);
  CT_EQ(pw.bl, 5);
  run_to(t0 + 30000 + 50 * 6);
  CT_EQ(pw.state, RA_PW_OFF);
  CT_EQ(pw.bl, 0);
  CT_EQ(fk.bl_w[fk.nbl - 1], 0);
  /* no step faster than 50 ms: ticks between steps write nothing */
  begin();
  t0 = fk.now;
  fk.now = t0 + 10000;
  ra_power_tick(&pw, fk.now);
  int n = fk.nbl;
  fk.now += 49;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nbl, n);
  fk.now += 1;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nbl, n + 1);
}

static void test_wake_consumes(void) {
  begin();
  uint64_t t0 = fk.now;
  /* input while active passes and restarts the idle timer */
  run_to(t0 + 8000);
  CT_CHECK(ra_power_input(&pw, fk.now, true));
  run_to(t0 + 8000 + 9950);
  CT_EQ(pw.state, RA_PW_ACTIVE);
  run_to(t0 + 8000 + 10000);
  CT_EQ(pw.state, RA_PW_DIM);
  /* activity() never wakes */
  ra_power_activity(&pw, fk.now);
  CT_EQ(pw.state, RA_PW_DIM);
  /* from dim: every press acts, and wakes */
  CT_CHECK(ra_power_input(&pw, fk.now, true));
  CT_EQ(pw.state, RA_PW_ACTIVE);
  CT_EQ(fk.led_w[fk.nled - 1], 42);
  CT_CHECK(ra_power_input(&pw, fk.now + 10, true)); /* the next one is real */
  /* from off, too, and the screen fades back up one step at a time */
  run_to(fk.now + 31000);
  CT_EQ(pw.state, RA_PW_OFF);
  CT_EQ(pw.bl, 0);
  CT_CHECK(ra_power_input(&pw, fk.now, false)); /* off: a hard key acts and wakes */
  CT_EQ(pw.state, RA_PW_ACTIVE);
  run_to(fk.now + 31000);
  CT_EQ(pw.state, RA_PW_OFF);
  CT_CHECK(!ra_power_input(&pw, fk.now, true)); /* off: a touch only wakes */
  int n = fk.nbl;
  run_to(fk.now + 50 * 26);
  CT_EQ(pw.bl, 26);
  CT_EQ(fk.nbl - n, 26);
  CT_EQ(fk.bl_w[fk.nbl - 1], 26);
}

static void batt_case(const char *soc, const char *src, const char *chg, int want_exec, const char *why) {
  begin();
  fk_set_file(&fk, SOC, soc);
  fk_set_file(&fk, SRC, src);
  fk_set_file(&fk, CHG, chg);
  fk.now += 100;
  ra_power_tick(&pw, fk.now); /* the first tick polls */
  if (fk.nexec != want_exec) fprintf(stderr, "battery case failed: %s\n", why);
  CT_EQ(fk.nexec, want_exec);
  CT_CHECK(fk_log_is_safe(&fk, NULL));
}

static void test_battery_guard(void) {
  /* the one condition that powers off */
  batt_case("2.5\n", "battery\n", "0.00 0x0000\n", 1, "exact");
  batt_case("3.0\n", "battery\n", "0.00 0x0000\n", 1, "boundary 3.0");
  batt_case("0.0\n", "battery", "0.00 0x0000", 1, "no trailing newline");
  batt_case("2.0\n", "battery\n", "-0.20 0x00ff\n", 1, "discharging current is negative");
  /* every way it must not */
  batt_case("3.1\n", "battery\n", "0.00 0x0000\n", 0, "soc 3.1");
  batt_case("100.0\n", "battery\n", "0.00 0x0000\n", 0, "full");
  batt_case("2.0\n", "usb\n", "0.00 0x0000\n", 0, "usb");
  batt_case("2.0\n", "cradle\n", "0.00 0x0000\n", 0, "cradle");
  batt_case("2.0\n", "wall_adapter\n", "0.00 0x0000\n", 0, "wall adapter");
  batt_case("2.0\n", "Battery\n", "0.00 0x0000\n", 0, "case matters");
  batt_case("2.0\n", "battery2\n", "0.00 0x0000\n", 0, "prefix");
  batt_case("2.0\n", "batter\n", "0.00 0x0000\n", 0, "short");
  batt_case("2.0\n", "\n", "0.00 0x0000\n", 0, "empty source");
  batt_case("2.0\n", "battery\n", "0.45 0x01d9\n", 0, "charging");
  batt_case("2.0\n", "battery\n", "garbage\n", 0, "charge garbage");
  batt_case("2.0\n", "battery\n", "\n", 0, "charge empty");
  batt_case("garbage\n", "battery\n", "0.00 0x0000\n", 0, "soc garbage");
  batt_case("\n", "battery\n", "0.00 0x0000\n", 0, "soc empty");
  batt_case("nan\n", "battery\n", "0.00 0x0000\n", 0, "soc nan");
  batt_case("-inf\n", "battery\n", "0.00 0x0000\n", 0, "soc -inf");
  batt_case("-5\n", "battery\n", "0.00 0x0000\n", 0, "soc negative");
  batt_case("2.0x\n", "battery\n", "0.00 0x0000\n", 0, "soc trailing junk");
  batt_case("2.0\n", "battery\n", "0.00x\n", 0, "charge trailing junk");
  /* missing files: each of the three */
  batt_case(NULL, "battery\n", "0.00 0x0000\n", 0, "soc missing");
  batt_case("2.0\n", NULL, "0.00 0x0000\n", 0, "powersource missing");
  batt_case("2.0\n", "battery\n", NULL, 0, "battcharge missing");

  /* a failed read logs and never powers off */
  begin();
  fk_set_file(&fk, SOC, NULL);
  fk.now += 100;
  ra_power_tick(&pw, fk.now);
  CT_CHECK(fk.nmsgs > 0);
  CT_EQ(fk.nexec, 0);
}

static void test_battery_fresh_each_poll(void) {
  begin();
  fk_set_file(&fk, SOC, "2.0\n");
  fk_set_file(&fk, SRC, "usb\n");
  fk.now += 100;
  ra_power_tick(&pw, fk.now); /* on usb: nothing */
  CT_EQ(fk.nexec, 0);
  /* the soc node vanishes: the earlier good soc must not be reused */
  fk_set_file(&fk, SRC, "battery\n");
  fk_set_file(&fk, SOC, NULL);
  fk.now += 60000;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nexec, 0);
  /* polls are 60 s apart */
  fk_set_file(&fk, SOC, "2.0\n");
  fk.now += 59999;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nexec, 0);
  fk.now += 1;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nexec, 1);
  /* once requested it is not requested again */
  fk.now += 60000;
  ra_power_tick(&pw, fk.now);
  CT_EQ(fk.nexec, 1);
  CT_EQ(fk_count(&fk, "exec /sbin/poweroff"), 1);
}

int main(void) {
  CT_RUN(test_constants);
  CT_RUN(test_idle_machine);
  CT_RUN(test_wake_consumes);
  CT_RUN(test_battery_guard);
  CT_RUN(test_battery_fresh_each_poll);
  return CT_DONE();
}
