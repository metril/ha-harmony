#ifndef HP_REMOTE_CLI_H
#define HP_REMOTE_CLI_H

/* harmony_remote command line:
 *   --rf spi|none     spi: the real radio (default on the device build); none: the null link, for
 *                     bench display tests (default on a host build)
 *   --powermode 0xN0  upper nibble OR'd into byte 1 of every frame. Default 0x00. UNVALIDATED: stock's
 *                     value was not recovered. The low nibble must be 0.
 *   --slot N          override the hub's slot on the remote, 0..15 (default 0 = learned from the radio's
 *                     link-up report, measured slot 1)
 *   --rf-debug        log every radio write/read frame and every link event to stderr (a flag, no value)
 *   --ui-debug        one stderr line per screen refresh (flush count, pixels, render/flush time, reason, and for
 *                     touches the input-event-to-flush latency) (a flag, no value)
 * Both `--opt value` and `--opt=value` are accepted. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  bool rf_spi;
  uint8_t powermode, slot;
  bool rf_debug, ui_debug;
} ra_cli_t;

/* 0 ok; -1 bad or unknown option / value (nothing is printed; the caller prints usage, exits 2). */
int ra_cli_parse(int argc, char **argv, ra_cli_t *out);
void ra_cli_usage(FILE *f, const char *prog);

#endif
