#ifndef HP_HOOKLOG_H
#define HP_HOOKLOG_H

/* Ships the boot hook's log (tmpfs /var/volatile/ha-harmony-hook.log, next to the --health-file) through the hub log
 * path, tagged "[boot-hook] ". Read once at start (it holds the hook's lines, including respawn notes written while
 * the daemon was down), then whenever inotify reports a write: no polling. The offset lives in memory only; a
 * truncated file (new boot) restarts from 0. The lines are not copied into the daemon log (they are already in a file). */

#include <stdbool.h>
#include <sys/types.h>

#include "hub/net/hublog.h"

typedef struct {
    int ino; /* inotify fd, -1 when unavailable (the file is then read once) */
    char path[256], offpath[264]; /* the hook log, and the tmpfs file remembering how much of it was shipped */
    off_t saved;
    off_t off;
    hp_linebuf_t lb;
    hp_hublog_t *log;
} hp_hooklog_t;

/* Reads what is there and starts watching. `log` NULL = nothing to ship to. Never fails the daemon. */
void hp_hooklog_start(hp_hooklog_t *, hp_hublog_t *log, const char *path);
/* Poll this fd for readability (-1: none); then call hp_hooklog_service. */
int hp_hooklog_fd(const hp_hooklog_t *);
/* Reads whatever was appended since the last call (and drains the inotify events). */
void hp_hooklog_service(hp_hooklog_t *);
/* "<dirname of file>/ha-harmony-hook.log" into out; false when file has no directory part or does not fit. */
bool hp_hooklog_path_beside(const char *file, char *out, size_t cap);

#endif
