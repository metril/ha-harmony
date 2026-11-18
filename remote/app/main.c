#define _POSIX_C_SOURCE 200809L
/* harmony_remote: the Harmony Elite remote app. Renders pages with LVGL on /dev/fb0, reads the hard
 * keys and touch from evdev, drives backlight/LEDs, guards the battery and speaks to the hub over
 * the RF link (--rf spi, remote/app/rf_link.h). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/app/app.h"
#include "remote/app/cli.h"
#include "remote/app/rf_link.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv) {
  ra_cli_t cli;
  if (ra_cli_parse(argc, argv, &cli) < 0) {
    ra_cli_usage(stderr, argv[0]);
    return 2;
  }
  fprintf(stderr, "harmony_remote: rf=%s powermode=0x%02X%s slot=%u\n", cli.rf_spi ? "spi" : "none", cli.powermode,
          cli.powermode ? "" : " (default, unvalidated)", cli.slot);

  struct sigaction sa = {.sa_handler = on_signal}; /* no SA_RESTART: select() returns EINTR */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  static app_t app; /* big (pages, widget tables): keep it off the stack */
  static rf_link_t rf;
  rf_link_init(&rf, &ra_real_ops, RF_LINK_REMOTE_ID);
  app_link_t link = cli.rf_spi ? rf_link_app(&rf) : app_link_null;
  int r = app_init(&app, &ra_real_ops, &link, getenv("HARMONY_BOOTCOUNT_FILE"));
  if (r < 0) {
    fprintf(stderr, "harmony_remote: init failed (%d)\n", r);
    return 1;
  }
  rf_link_set_store(&rf, &app.store);
  /* Suspend to RAM is opt-in until the bench gate passes (docs/runbooks/remote-suspend-soak.md). */
  const char *sm = getenv("HARMONY_SUSPEND");
  if (sm && !strcmp(sm, "battery")) app.power.suspend_mode = RA_SUSPEND_BATTERY;
  else if (sm && !strcmp(sm, "always")) app.power.suspend_mode = RA_SUSPEND_ALWAYS;
  app.display.ui_debug = cli.ui_debug;
  if (cli.rf_spi) {
    /* After the panel checks, so a refused start never touches the radio. A failed open (stock hal
     * alive, no device) leaves the link dead: the fallback page shows and the app keeps running. */
    hp_rf_spi_cfg_t cfg = {.slot = cli.slot, .powermode = cli.powermode, .role = HP_RF_SPI_ROLE_REMOTE, .debug = cli.rf_debug};
    int rc = rf_link_open(&rf, &cfg);
    if (rc < 0) fprintf(stderr, "harmony_remote: cannot open the radio (%d); running without a link\n", rc);
  }
  while (!g_stop) app_step(&app, APP_LINK_IDLE_WAIT_MS);
  rf_link_close(&rf); /* joins the reader before the radio's fd goes: never open without a reader */
  app_destroy(&app);
  return 0;
}
