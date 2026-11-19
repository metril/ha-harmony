#include "remote/app/cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool parse_uint(const char *s, unsigned long max, unsigned long *out) {
  if (!isdigit((unsigned char)*s)) return false; /* no sign, no space, not empty */
  char *end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 0);
  if (errno || *end || v > max) return false;
  *out = v;
  return true;
}

int ra_cli_parse(int argc, char **argv, ra_cli_t *out) {
  ra_cli_t c = {
#ifdef HP_REMOTE_DEVICE
      .rf_spi = true,
#else
      .rf_spi = false,
#endif
  };
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = NULL;
    size_t n = 0;
    while (a[n] && a[n] != '=') n++;
    if (n == 10 && !strncmp(a, "--rf-debug", 10)) { /* a flag: no value */
      if (a[n] == '=') return -1;
      c.rf_debug = true;
      continue;
    }
    if (n == 10 && !strncmp(a, "--ui-debug", 10)) {
      if (a[n] == '=') return -1;
      c.ui_debug = true;
      continue;
    }
    if (a[n] == '=') v = a + n + 1;
    else if (i + 1 < argc) v = argv[i + 1], i++;
    if (!v) return -1;
    unsigned long x;
    if (n == 4 && !strncmp(a, "--rf", 4)) {
      if (!strcmp(v, "spi")) c.rf_spi = true;
      else if (!strcmp(v, "none")) c.rf_spi = false;
      else return -1;
    } else if (n == 11 && !strncmp(a, "--powermode", 11)) {
      if (!parse_uint(v, 0xFF, &x) || (x & 0x0F)) return -1;
      c.powermode = (uint8_t)x;
    } else if (n == 6 && !strncmp(a, "--slot", 6)) {
      if (!parse_uint(v, 15, &x)) return -1;
      c.slot = (uint8_t)x;
    } else {
      return -1;
    }
  }
  *out = c;
  return 0;
}

void ra_cli_usage(FILE *f, const char *prog) {
  fprintf(f,
          "usage: %s [--rf spi|none] [--powermode 0xN0] [--slot N] [--rf-debug] [--ui-debug]\n"
          "  --rf         spi = the radio (device default), none = no link (bench display tests)\n"
          "  --powermode  upper nibble OR'd into frame byte 1, 0x00..0xF0 (default 0x00, UNVALIDATED)\n"
          "  --slot       override the hub's slot on the remote, 1..15 (default: learned from the link-up report)\n"
          "  --rf-debug   log every radio frame and link event to stderr\n"
          "  --ui-debug   one stderr line per screen refresh: flushes, pixels, render/flush time, why, touch-to-flush ms\n",
          prog);
}
