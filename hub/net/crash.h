#ifndef HP_CRASH_H
#define HP_CRASH_H

/* Why did the daemon die? A fatal signal (SEGV, ABRT, BUS, FPE, ILL) writes one short line to the log fd (the tmpfs
 * daemon log) and appends it to the boot hook's tmpfs log, which the respawned daemon ships to Home Assistant as
 * "[boot-hook] ...". The line carries the signal and the last few breadcrumbs: every hp_log line plus the hub commands
 * and IR sends (hp_crumb). Nothing here touches flash; the handler uses only async-signal-safe calls, then re-raises
 * so the exit status the boot hook records is still "killed by signal N". */

#include <stddef.h>

#define HP_CRUMBS 8
#define HP_CRUMB_LEN 80

/* Remember one line (newline stripped, cut to HP_CRUMB_LEN). Any thread, never blocks, no file I/O. */
void hp_crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hp_crumb_raw(const char *s, size_t n);
/* Install the fatal-signal handlers. hook_log: the boot hook's log path (NULL = log fd only). */
void hp_crash_install(const char *hook_log);
/* Test hook: the formatted report for `sig` into out (NUL-terminated); returns its length. */
size_t hp_crash_format(int sig, char *out, size_t cap);

#endif
