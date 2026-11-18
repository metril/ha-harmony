/* App level: refusal on a wrong panel, fallback only on link failure, boot-counter reset, and the
 * brick-safety allow-list over a whole simulated session. */
#include "remote_fake.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;

/* page 1, 4x6: button id 1 "Vol+" at cell (0,0) */
static const uint8_t PAGE1[] = {0x01, 0x01, 0x04, 0x06, 0x01, 0x00, 0x01, 0x00, 0x11, 0x04, 'V', 'o', 'l', '+'};
/* page 2, 4x6: tile id 5 at (1,1) 2x1 */
static const uint8_t PAGE2[] = {0x01, 0x02, 0x04, 0x06, 0x01, 0x01, 0x05, 0x11, 0x21, 0x00};

static int start(const char *bootcount) {
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  return app_init(&app, &ops, &l, bootcount);
}

static void step_at(uint64_t t) {
  fk.now = t;
  app_step(&app, 0);
}

static void test_refuses_wrong_panel(void) {
  fk_init(&fk);
  fk.var.yres = 240; fk.var.xres = 320; /* landscape */
  ra_ops_t ops = fk_ops(&fk);
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), -2);
  CT_EQ(fk_count(&fk, "mmap"), 0);
  CT_EQ(fk.nbl, 0);
  CT_CHECK(fk.nmsgs > 0);

  fk_init(&fk);
  fk.var.red = (struct fb_bitfield){0, 5, 0}; /* BGR565 */
  fk.var.blue = (struct fb_bitfield){11, 5, 0};
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), -2);
  fk_init(&fk);
  fk.var.bits_per_pixel = 32;
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), -2);
  CT_EQ(fk_count(&fk, "mmap"), 0);
  /* the two refusals never opened input or touched sysfs */
  CT_EQ(fk_count(&fk, "open /dev/input"), 0);
  CT_EQ(fk_count(&fk, "open /sys"), 0);

  /* and the healthy panel starts */
  CT_EQ(start(NULL), 0);
  app_destroy(&app);
}

static void test_fallback_only_on_link_failure(void) {
  CT_EQ(start(NULL), 0);
  step_at(1000);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_BOOT); /* boot page before any push */
  CT_CHECK(!app.ui.fallback);
  fkl_push_page(&lk, PAGE1, sizeof PAGE1);
  step_at(1100);
  CT_EQ(app.ui.page.page_id, 1);
  /* five minutes of silence, through dim and off: still the page, never the fallback */
  for (uint64_t t = 2000; t < 300000; t += 1000) step_at(t);
  CT_CHECK(!app.ui.fallback);
  CT_EQ(app.ui.page.page_id, 1);
  CT_EQ(app.power.state, RA_PW_OFF);
  /* a state_update and a bad page do not trip it either */
  fkl_push_kind(&lk, APP_LINK_EV_STATE_UPDATE);
  uint8_t bad[] = {0x09, 0, 1, 1, 0};
  fkl_push_page(&lk, bad, sizeof bad);
  step_at(301000);
  CT_CHECK(!app.ui.fallback);
  CT_EQ(app.ui.page.page_id, 1);
  CT_EQ(app.state_updates, 1);
  /* a send failure alone does not: it takes 3 in a row AND 5 s without a word from the hub */
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step_at(302000); /* the state_update above counts as the hub speaking, at 301000 */
  CT_CHECK(!app.ui.fallback);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step_at(306100);
  CT_CHECK(app.ui.fallback);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_FALLBACK);
  /* any hub frame brings the last real page back (tile updates meanwhile land in the saved copy) */
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  step_at(306200);
  CT_CHECK(!app.ui.fallback);
  CT_EQ(app.ui.page.page_id, 1);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step_at(312000);
  CT_CHECK(app.ui.fallback);
  /* a successful object in between resets the run: heard = alive */
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  step_at(312100);
  CT_CHECK(!app.ui.fallback);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step_at(330000);
  CT_CHECK(!app.ui.fallback);
  /* a fatal failure (no radio at all) shows it at once */
  app_link_event_t *fe = &lk.q[lk.nq++];
  memset(fe, 0, sizeof *fe);
  fe->kind = APP_LINK_EV_FAILED;
  fe->fatal = true;
  step_at(330100);
  CT_CHECK(app.ui.fallback);
  /* a fresh page pushed after recovery replaces it */
  fkl_push_page(&lk, PAGE2, sizeof PAGE2);
  step_at(303000);
  CT_CHECK(!app.ui.fallback);
  CT_EQ(app.ui.page.page_id, 2);
  /* tile updates by id, for the page on screen only */
  app_link_event_t *e = &lk.q[lk.nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_TILE_UPDATE;
  e->page = 2; e->tile = 5;
  strcpy(e->text, "72 F");
  step_at(304000);
  CT_CHECK(!strcmp(app.ui.page.widgets[0].text, "72 F"));
  app_destroy(&app);
}

static void test_unblank_and_awake(void) {
  CT_EQ(start(NULL), 0);
  CT_EQ(fk.nblank, 1); /* the panel stays dark until FBIOBLANK(UNBLANK) */
  CT_EQ(fk.blank_arg, FB_BLANK_UNBLANK);
  CT_CHECK(fk_log_is_safe(&fk, NULL));
  step_at(1000);
  CT_EQ(lk.nawake, 1);
  CT_CHECK(lk.awake); /* awake while the backlight is on */
  for (uint64_t t = 2000; t < 40000; t += 1000) step_at(t);
  CT_EQ(app.power.state, RA_PW_OFF);
  CT_CHECK(!lk.awake); /* asleep: the link must not poll */
  int n = lk.nawake;
  step_at(41000);
  CT_EQ(lk.nawake, n); /* only changes are reported */
  fk_key(&fk, 0, 115, 1); /* a press wakes it */
  step_at(42000);
  CT_CHECK(lk.awake);
  app_destroy(&app);
}

static void test_bootcount(void) {
  CT_EQ(start("/tmp/bootcount"), 0);
  CT_EQ(fk.boot_writes, 0); /* not before a frame has been drawn */
  step_at(1000);
  CT_EQ(fk.boot_writes, 1);
  CT_CHECK(!strcmp(fk.bootcount, "0\n"));
  step_at(2000);
  step_at(3000);
  CT_EQ(fk.boot_writes, 1); /* once */
  app_destroy(&app);
  /* unset: nothing is opened for it */
  CT_EQ(start(NULL), 0);
  step_at(1000);
  CT_EQ(fk.boot_writes, 0);
  CT_EQ(fk_count(&fk, "open /tmp"), 0);
  app_destroy(&app);
}

static void test_seam_refuses(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  ra_sys_t s;
  ra_sys_init(&s, &ops, "/tmp/bootcount");
  const char *bad_in[] = {"/dev/rfspi", "/dev/rffw", "/dev/mspfw", "/dev/watchdog", "/dev/mtd0", "/dev/mtdblock7",
                          "/dev/input/event1", "/dev/input/event3", "/dev/fb0", "/etc/passwd"};
  for (unsigned i = 0; i < sizeof bad_in / sizeof bad_in[0]; i++) {
    errno = 0;
    CT_EQ(ra_open_input(&s, bad_in[i]), -1);
    CT_EQ(errno, EPERM);
  }
  const char *bad_w[] = {"/sys/power/state", "/sys/bus/i2c/devices/1-0034/kill", "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/charger",
                         "/sys/bus/i2c/devices/1-0034/wakeup", "/dev/rfspi", RA_PATH_SOC, "/tmp/bootcount"};
  for (unsigned i = 0; i < sizeof bad_w / sizeof bad_w[0]; i++) {
    errno = 0;
    CT_EQ(ra_sysfs_write(&s, bad_w[i], "1"), -1);
    CT_EQ(errno, EPERM);
  }
  char buf[16];
  const char *bad_r[] = {"/sys/power/state", "/sys/bus/i2c/devices/1-0034/kill", "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/charger",
                         RA_PATH_BACKLIGHT, "/dev/mtd0"};
  for (unsigned i = 0; i < sizeof bad_r / sizeof bad_r[0]; i++) {
    errno = 0;
    CT_EQ(ra_sysfs_read(&s, bad_r[i], buf, sizeof buf), -1);
    CT_EQ(errno, EPERM);
  }
  /* no fb open yet: ioctl and mmap are refused; afterwards only the two GETs pass */
  CT_EQ(ra_ioctl_fb(&s, FBIOGET_VSCREENINFO, buf), -1);
  CT_CHECK(ra_mmap_fb(&s, 4096) == NULL);
  CT_CHECK(ra_open_fb(&s) >= 0);
  struct fb_var_screeninfo v;
  CT_EQ(ra_ioctl_fb(&s, FBIOGET_VSCREENINFO, &v), 0);
  CT_EQ(ra_ioctl_fb(&s, FBIOPUT_VSCREENINFO, &v), -1);
  CT_EQ(ra_ioctl_fb(&s, FBIOPAN_DISPLAY, &v), -1);
  CT_EQ(ra_ioctl_fb(&s, FBIOBLANK, &v), -1);
  CT_EQ(ra_ioctl_fb(&s, EVIOCGRAB, &v), -1);
  /* nothing refused ever reached the ops table */
  CT_EQ(fk_count(&fk, "open /dev/rfspi"), 0);
  CT_EQ(fk_count(&fk, "open /sys"), 0);
  CT_EQ(fk_count(&fk, "open /dev/m"), 0);
  CT_EQ(fk_count(&fk, "ioctl"), 1);
  CT_CHECK(fk_log_is_safe(&fk, "/tmp/bootcount"));
}

static void test_full_session_is_safe(void) {
  CT_EQ(start("/tmp/bootcount"), 0);
  uint64_t t = 1000;
  step_at(t);
  fkl_push_page(&lk, PAGE1, sizeof PAGE1);
  t += 100;
  step_at(t);
  CT_EQ(app.ui.page.page_id, 1);

  /* hard key: VolumeUp press + release */
  fk_key(&fk, 0, 115, 1);
  t += 100;
  step_at(t);
  fk_key(&fk, 0, 115, 0);
  t += 100;
  step_at(t);
  /* tap on the button */
  fk_touch(&fk, 10, 42, 1);
  t += 100;
  step_at(t);
  fk_touch(&fk, 10, 42, 0);
  t += 100;
  step_at(t);
  /* a held key: press, two holds, release */
  fk_key(&fk, 0, 114, 1);
  t += 50;
  step_at(t);
  t += 700;
  step_at(t);
  fk_key(&fk, 0, 114, 0);
  t += 50;
  step_at(t);
  CT_EQ(lk.nsent, 2 + 2 + 3); /* VolumeUp, tap, VolumeDown press+hold+release */
  int sent_before_idle = lk.nsent;
  CT_EQ(lk.sent[0].code, hp_hwkey_by_name("VolumeUp")->id);
  CT_EQ(lk.sent[2].code, HP_SOFT_KEY_BASE + 1);
  CT_EQ(lk.sent[sent_before_idle - 1].action, HP_KEY_ACTION_RELEASE);

  /* idle to dim, then off */
  t += 11000;
  step_at(t);
  CT_EQ(app.power.state, RA_PW_DIM);
  t += 20000;
  step_at(t);
  CT_EQ(app.power.state, RA_PW_OFF);
  /* wake: the press that wakes is consumed, then normal again */
  fk_key(&fk, 0, 115, 1);
  fk_key(&fk, 0, 115, 0);
  t += 100;
  step_at(t);
  CT_EQ(app.power.state, RA_PW_ACTIVE);
  CT_EQ(lk.nsent, sent_before_idle);
  fk_key(&fk, 0, 115, 1);
  fk_key(&fk, 0, 115, 0);
  t += 100;
  step_at(t);
  CT_EQ(lk.nsent, sent_before_idle + 2);

  /* battery poll on a healthy cradle: no poweroff */
  t += 61000;
  step_at(t);
  CT_EQ(fk.nexec, 0);
  /* and the exact low-battery condition: one poweroff, via the allow-listed path only */
  fk_set_file(&fk, RA_PATH_SOC, "2.0\n");
  fk_set_file(&fk, RA_PATH_POWERSRC, "battery\n");
  t += 60000;
  step_at(t);
  CT_EQ(fk.nexec, 1);
  t += 60000;
  step_at(t);
  CT_EQ(fk.nexec, 1);

  CT_CHECK(fk_log_is_safe(&fk, "/tmp/bootcount"));
  const char *never[] = {"rfspi", "rffw", "mspfw", "watchdog", "mtd", "power/state", "kill", "charger", "event1", "event3"};
  for (int i = 0; i < fk.nlog; i++)
    for (unsigned j = 0; j < sizeof never / sizeof never[0]; j++)
      if (strstr(fk.log[i], never[j])) { fprintf(stderr, "forbidden: %s\n", fk.log[i]); CT_CHECK(0); }
  CT_EQ(fk.boot_writes, 1);
  app_destroy(&app);
}

static void test_status_bar_and_state_screens(void) {
  CT_EQ(start(NULL), 0);
  step_at(1000);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_BOOT); /* connecting */
  CT_EQ(app.ui.soc, 100);                      /* the first gauge poll fed the bar */
  CT_EQ(app.ui.charging, 1);                   /* "cradle" is external power */
  CT_EQ(app.ui.link_lost, 0);
  /* the hub speaks before any page: receiving config */
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  step_at(1100);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  /* a failed send flags the link, any frame from the hub clears it again */
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step_at(1200);
  CT_EQ(app.ui.link_lost, 1);
  CT_CHECK(!app.ui.fallback); /* the fallback policy is untouched: 1 failure is not 3 */
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  step_at(1300);
  CT_EQ(app.ui.link_lost, 0);
  fkl_push_page(&lk, PAGE1, sizeof PAGE1);
  step_at(1400);
  CT_EQ(app.ui.page.page_id, 1);
  /* touch down shows the pressed widget, release clears it; a miss presses nothing */
  fk_touch(&fk, 10, 42, 1);
  step_at(1500);
  CT_EQ(app.ui.pressed, 0);
  fk_touch(&fk, 10, 42, 0);
  step_at(1600);
  CT_EQ(app.ui.pressed, -1);
  fk_touch(&fk, 230, 300, 1);
  step_at(1700);
  CT_EQ(app.ui.pressed, -1);
  fk_touch(&fk, 230, 300, 0);
  step_at(1800);
  /* low battery on battery power: a toast, once per dip; any touch clears it */
  fk_set_file(&fk, RA_PATH_SOC, "8.0\n");
  fk_set_file(&fk, RA_PATH_POWERSRC, "battery\n");
  fk_set_file(&fk, RA_PATH_BATTCHARGE, "-100.00 0x0000\n");
  step_at(1000 + 60000);
  CT_EQ(app.ui.soc, 8);
  CT_EQ(app.ui.charging, 0);
  CT_CHECK(app.ui.toast != NULL);
  step_at(1000 + 120000); /* the next poll does not raise it again... */
  CT_CHECK(app.ui.toast == NULL); /* ...and the first one timed out */
  fk_set_file(&fk, RA_PATH_SOC, "9.0\n");
  step_at(1000 + 125000);
  CT_CHECK(app.ui.toast == NULL);
  fk_set_file(&fk, RA_PATH_SOC, "50.0\n"); /* recovered, then dips again: re-armed */
  step_at(1000 + 180000);
  fk_set_file(&fk, RA_PATH_SOC, "7.0\n");
  step_at(1000 + 240000);
  CT_CHECK(app.ui.toast != NULL);
  fk_touch(&fk, 230, 300, 1); /* the screen was off: this touch only wakes it, the toast stays */
  fk_touch(&fk, 230, 300, 0);
  step_at(1000 + 240100);
  CT_CHECK(app.ui.toast != NULL);
  fk_touch(&fk, 230, 300, 1); /* a touch on the awake screen clears it */
  fk_touch(&fk, 230, 300, 0);
  step_at(1000 + 240200);
  CT_CHECK(app.ui.toast == NULL);
  CT_EQ(fk.nexec, 0); /* 7% is a warning, not the 3% poweroff guard */
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_refuses_wrong_panel);
  CT_RUN(test_fallback_only_on_link_failure);
  CT_RUN(test_status_bar_and_state_screens);
  CT_RUN(test_unblank_and_awake);
  CT_RUN(test_bootcount);
  CT_RUN(test_seam_refuses);
  CT_RUN(test_full_session_is_safe);
  return CT_DONE();
}
