#define _GNU_SOURCE
#include "hub/net/hooklog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "[boot-hook] "

bool hp_hooklog_path_beside(const char *file, char *out, size_t cap) {
    const char *sl = file ? strrchr(file, '/') : NULL;
    if (!sl) return false;
    return snprintf(out, cap, "%.*s/ha-harmony-hook.log", (int)(sl - file), file) < (int)cap;
}

static void read_new(hp_hooklog_t *h) {
    int fd = open(h->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < h->off) { /* truncated: a new boot's log */
        h->off = 0;
        h->lb.n = 0;
        h->lb.over = false;
    }
    char b[512];
    for (;;) {
        ssize_t n = pread(fd, b, sizeof b, h->off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        h->off += n;
        hp_hublog_feed_ship(h->log, &h->lb, TAG, b, (size_t)n);
    }
    close(fd);
}

void hp_hooklog_start(hp_hooklog_t *h, hp_hublog_t *log, const char *path) {
    memset(h, 0, sizeof *h);
    h->ino = -1;
    h->log = log;
    if (!log || !path || snprintf(h->path, sizeof h->path, "%s", path) >= (int)sizeof h->path) return;
    h->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (h->ino >= 0 && inotify_add_watch(h->ino, h->path, IN_MODIFY | IN_CLOSE_WRITE) < 0) {
        close(h->ino); /* no such file yet: shipped once below, nothing to watch */
        h->ino = -1;
    }
    read_new(h); /* after the watch exists: a write in between is not missed */
}

int hp_hooklog_fd(const hp_hooklog_t *h) { return h->ino; }

void hp_hooklog_service(hp_hooklog_t *h) {
    if (h->ino < 0) return;
    char junk[512];
    while (read(h->ino, junk, sizeof junk) > 0) {}
    read_new(h);
}
