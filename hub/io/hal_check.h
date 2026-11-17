#ifndef HP_IO_HAL_CHECK_H
#define HP_IO_HAL_CHECK_H

/* Is the stock `hal` process alive? Header-only so tools/rf_probe.c (a single-file tool) and the
 * hub share one implementation. Walks /proc/<pid>/stat and reads the name between the first '('
 * and the last ')': the hub's 2.6.31 kernel has no /proc/<pid>/comm and its BusyBox no pgrep.
 * 1 = a process named exactly `hal` is alive, 0 = none, -1 = /proc unreadable (indeterminate;
 * callers must treat it as alive). An unreadable single entry is skipped (a pid can exit
 * mid-walk). Only opens /proc. */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline int hp_hal_alive_proc(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        const char *n = e->d_name;
        size_t i = 0;
        while (n[i] >= '0' && n[i] <= '9') i++;
        if (i == 0 || n[i] != 0) continue; /* not a pid directory */
        char path[64], buf[256];
        if (snprintf(path, sizeof path, "/proc/%s/stat", n) >= (int)sizeof path) continue;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t got = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (got <= 0) continue;
        buf[got] = 0;
        char *a = strchr(buf, '('), *b = strrchr(buf, ')');
        if (!a || !b || b <= a + 1) continue;
        *b = 0;
        found = strcmp(a + 1, "hal") == 0;
    }
    closedir(d);
    return found;
}

#endif
