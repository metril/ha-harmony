#define _POSIX_C_SOURCE 200809L
/* harmony_remote: the Harmony Elite remote app. Renders pages with LVGL on /dev/fb0, reads the hard
 * keys and touch from evdev, drives backlight/LEDs and guards the battery. The RF link is a no-op
 * until Task 3 plugs it in (app_link_t). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "remote/app/app.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

int main(void) {
  struct sigaction sa = {.sa_handler = on_signal}; /* no SA_RESTART: select() returns EINTR */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  static app_t app; /* big (pages, widget tables): keep it off the stack */
  int r = app_init(&app, &ra_real_ops, &app_link_null, getenv("HARMONY_BOOTCOUNT_FILE"));
  if (r < 0) {
    fprintf(stderr, "harmony_remote: init failed (%d)\n", r);
    return 1;
  }
  while (!g_stop) app_step(&app, 1000);
  app_destroy(&app);
  return 0;
}
