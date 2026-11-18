#define _DEFAULT_SOURCE 1
#include <unistd.h>
/* Suspend to RAM and the battery guard around it, with an injected clock and a fake /sys/power/state. */
#include "remote_fake.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;
static uint64_t t;

static void begin(ra_suspend_mode_t mode, const char *src) {
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  CT_EQ(app_init(&app, &ops, &l, NULL), 0);
  app.power.suspend_mode = mode;
  fk_set_file(&fk, RA_PATH_POWERSRC, src);
  fk_set_file(&fk, RA_PATH_SOC, "80.0\n");
  t = fk.now;
}
static void step(uint64_t dt) {
  t += dt;
  fk.now = t;
  app_step(&app, 0);
}
static void idle(uint64_t ms) {
  for (uint64_t e = t + ms; t < e;) step(50);
}

static void test_default_never_suspends(void) {
  begin(RA_SUSPEND_NEVER, "battery\n");
  idle(120000);
  CT_EQ(app.power.state, RA_PW_OFF);
  CT_EQ(fk.nsuspend, 0);
  app_destroy(&app);
}

static void test_battery_suspend_and_resume(void) {
  begin(RA_SUSPEND_BATTERY, "battery\n");
  fk.suspend_sleep_ms = 600000;
  idle(RA_OFF_AFTER_MS - 1000);
  CT_EQ(fk.nsuspend, 0);
  idle(RA_SUSPEND_AFTER_MS - 1000); /* off, but not for RA_SUSPEND_AFTER_MS yet */
  CT_EQ(fk.nsuspend, 0);
  idle(3000);
  CT_EQ(fk.nsuspend, 1);
  CT_EQ(fk.suspend_order[0], 4); /* FB_BLANK_POWERDOWN was the level in force at the write */
  CT_EQ(app.power.suspends, 1);
  CT_EQ(app.power.suspended_ms, 600000); /* the wall clock saw it, the monotonic clock did not */
  CT_EQ(lk.nresumed, 1);
  CT_EQ(lk.last_gap, 600000);
  CT_EQ(fk.blank_arg, 0); /* unblanked again */
  CT_EQ(app.power.state, RA_PW_ACTIVE);
  CT_EQ(lk.awake, 1);
  CT_EQ(app.fail_run, 0);
  CT_CHECK(!app.ui.fallback); /* the silent sleep is not a link failure */
  int n = fk.nsuspend;
  idle(RA_SUSPEND_WAKE_HOLD_MS - 500);
  CT_EQ(fk.nsuspend, n); /* held awake to re-link */
  idle(RA_OFF_AFTER_MS + RA_SUSPEND_AFTER_MS + 3000);
  CT_EQ(fk.nsuspend, 2);
  CT_EQ(app.power.suspended_ms, 1200000);
  CT_CHECK(fk_log_is_safe(&fk, NULL));
  for (int i = 0; i < fk.nlog; i++) CT_CHECK(!strstr(fk.log[i], "cpu"));
  app_destroy(&app);
}

static void test_powered_never_suspends_in_battery_mode(void) {
  const char *src[] = {"cradle\n", "usb\n", "garbage\n"};
  for (int i = 0; i < 3; i++) {
    begin(RA_SUSPEND_BATTERY, src[i]);
    idle(120000);
    CT_EQ(app.power.state, RA_PW_OFF);
    CT_EQ(fk.nsuspend, 0);
    fk_set_file(&fk, RA_PATH_POWERSRC, "battery\n"); /* unplugged later: the re-check suspends */
    idle(RA_SUSPEND_RECHECK_MS + 2000);
    CT_EQ(fk.nsuspend, 1);
    app_destroy(&app);
  }
  begin(RA_SUSPEND_BATTERY, "cradle\n");
  fk_set_file(&fk, RA_PATH_POWERSRC, NULL); /* unreadable: never on a guess */
  idle(120000);
  CT_EQ(fk.nsuspend, 0);
  app_destroy(&app);
}

static void test_always_mode_for_bench(void) {
  begin(RA_SUSPEND_ALWAYS, "usb\n");
  idle(60000);
  CT_EQ(fk.nsuspend, 1);
  app_destroy(&app);
}

static void test_refused_suspend_backs_off(void) {
  begin(RA_SUSPEND_BATTERY, "battery\n");
  fk.suspend_errno = EBUSY;
  idle(45000);
  CT_EQ(fk.nsuspend, 0);
  CT_EQ(fk.blank_arg, 0); /* panel restored */
  CT_EQ(app.power.suspends, 0);
  CT_EQ(app.power.state, RA_PW_OFF);
  fk.suspend_errno = 0;
  idle(RA_SUSPEND_RETRY_MS - 15000);
  CT_EQ(fk.nsuspend, 0); /* still backing off */
  idle(20000);
  CT_EQ(fk.nsuspend, 1);
  app_destroy(&app);
}

static void test_wake_key_reaches_hub(void) {
  begin(RA_SUSPEND_BATTERY, "battery\n");
  idle(45000);
  CT_EQ(fk.nsuspend, 1);
  int before = lk.nsent;
  fk_key(&fk, 0, 115, 1); /* the key that ended the suspend */
  fk_key(&fk, 0, 115, 0);
  step(50);
  CT_EQ(lk.nsent, before + 2); /* the resume made the screen active, so the key goes on to the hub */
  app_destroy(&app);
}

static void test_gap_math(void) {
  CT_EQ(ra_power_gap_ms(1000, 500, 61000, 1500), 59000);
  CT_EQ(ra_power_gap_ms(1000, 500, 1000, 500), 0);
  CT_EQ(ra_power_gap_ms(5000, 0, 4000, 10), 0);                 /* wall clock stepped back */
  CT_EQ(ra_power_gap_ms(0, 0, 15ULL * 24 * 3600 * 1000, 0), 0); /* implausible: a clock jump */
  CT_EQ(ra_power_gap_ms(1000, 0, 1500, 900), 0);                /* never negative */
}

/* Forced low SoC: the guard acts on the first poll after a resume, cleanly, once, and the remote never sleeps again. */
static void test_low_soc_guard_after_resume(void) {
  begin(RA_SUSPEND_BATTERY, "battery\n");
  fk.resume_soc = "2.0\n"; /* drained while asleep */
  fk.resume_chg = "-120.00 0x0000\n";
  idle(45000);
  CT_EQ(fk.nsuspend, 1);
  CT_EQ(fk.nexec, 1);
  CT_EQ(fk_count(&fk, "exec /sbin/poweroff"), 1);
  int n = fk.nsuspend;
  idle(300000);
  CT_EQ(fk.nexec, 1);
  CT_EQ(fk.nsuspend, n);
  CT_CHECK(fk_log_is_safe(&fk, NULL));
  app_destroy(&app);
}

static void test_low_soc_hook_in_real_ops(void) {
  char dir[] = "/tmp/hp_fake_batt_XXXXXX";
  CT_CHECK(mkdtemp(dir) != NULL);
  const char *names[] = {"soc", "powersource", "battcharge"}, *vals[] = {"1.5\n", "battery\n", "-50.00 0x0\n"};
  for (int i = 0; i < 3; i++) {
    char p[96];
    snprintf(p, sizeof p, "%s/%s", dir, names[i]);
    FILE *f = fopen(p, "w");
    CT_CHECK(f != NULL);
    fputs(vals[i], f);
    fclose(f);
  }
  ra_sys_t s;
  ra_sys_init(&s, &ra_real_ops, NULL);
  char b[64], c[64], d[64];
  unsetenv("HARMONY_FAKE_BATT_DIR"); /* off by default: our files are not read */
  CT_CHECK(ra_sysfs_read(&s, RA_PATH_SOC, b, sizeof b) < 0 || strcmp(b, "1.5\n") != 0);
  setenv("HARMONY_FAKE_BATT_DIR", dir, 1);
  CT_CHECK(ra_sysfs_read(&s, RA_PATH_SOC, b, sizeof b) > 0);
  CT_CHECK(ra_sysfs_read(&s, RA_PATH_POWERSRC, c, sizeof c) > 0);
  CT_CHECK(ra_sysfs_read(&s, RA_PATH_BATTCHARGE, d, sizeof d) > 0);
  CT_EQ(ra_battery_should_poweroff(b, c, d), 1);
  unsetenv("HARMONY_FAKE_BATT_DIR");
  for (int i = 0; i < 3; i++) {
    char p[96];
    snprintf(p, sizeof p, "%s/%s", dir, names[i]);
    remove(p);
  }
  rmdir(dir);
}

static void test_suspend_only_via_ra_suspend(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  ra_sys_t s;
  ra_sys_init(&s, &ops, NULL);
  char buf[8];
  CT_EQ(ra_sysfs_write(&s, "/sys/power/state", "mem"), -1); /* the generic writer still refuses it */
  CT_EQ(errno, EPERM);
  CT_EQ(ra_sysfs_read(&s, "/sys/power/state", buf, sizeof buf), -1);
  CT_EQ(ra_fb_blank(&s, 1), -1); /* no fb open */
  CT_EQ(errno, EPERM);
}

int main(void) {
  CT_RUN(test_default_never_suspends);
  CT_RUN(test_battery_suspend_and_resume);
  CT_RUN(test_powered_never_suspends_in_battery_mode);
  CT_RUN(test_always_mode_for_bench);
  CT_RUN(test_refused_suspend_backs_off);
  CT_RUN(test_wake_key_reaches_hub);
  CT_RUN(test_gap_math);
  CT_RUN(test_low_soc_guard_after_resume);
  CT_RUN(test_low_soc_hook_in_real_ops);
  CT_RUN(test_suspend_only_via_ra_suspend);
  return CT_DONE();
}
