#define _POSIX_C_SOURCE 200809L
#include "hub/engine/atomic_file.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hub/hp_errors.h"

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int hp_atomic_write(const char *path, const uint8_t *data, size_t len) {
    if (!path || !*path || (!data && len)) return -HP_E_INVAL;
    char *tmp = malloc(strlen(path) + 32);
    if (!tmp) return -HP_E_IO;
    sprintf(tmp, "%s.tmp.%ld", path, (long)getpid());
    int rc = -HP_E_IO;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        bool ok = write_all(fd, data, len) == 0 && fsync(fd) == 0;
        ok = (close(fd) == 0) && ok;
        if (ok && rename(tmp, path) == 0) {
            rc = 0;
            char *dup = strdup(path); /* best-effort: persist the rename */
            if (dup) {
                int dfd = open(dirname(dup), O_RDONLY);
                if (dfd >= 0) {
                    (void)fsync(dfd);
                    close(dfd);
                }
                free(dup);
            }
        } else {
            unlink(tmp);
        }
    }
    free(tmp);
    return rc;
}
