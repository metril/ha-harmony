#define _GNU_SOURCE
#include "hub/net/hooklog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The shipped offset goes to tmpfs (RAM) so a respawned daemon does not send the same lines again. */
static void save_off(hp_hooklog_t *h) {
    off_t o = h->off - (off_t)h->lb.n; /* a half-read last line is read again */
    if (o == h->saved || !h->offpath[0]) return;
    char b[24];
    int k = snprintf(b, sizeof b, "%lld\n", (long long)o);
    int fd = open(h->offpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, b, (size_t)k) == k) h->saved = o;
    close(fd);
}

static void load_off(hp_hooklog_t *h) {
    char b[24] = "";
    int fd = h->offpath[0] ? open(h->offpath, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    long long v = n > 0 ? atoll(b) : 0;
    if (v > 0) h->off = h->saved = (off_t)v; /* read_new resets it when the log is shorter (a new boot) */
}

static void read_new(hp_hooklog_t *h) {
    int fd = open(h->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < h->off) { /* truncated: a new boot's log */
        h->off = 0;
        h->saved = -1;
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
    save_off(h);
}

void hp_hooklog_start(hp_hooklog_t *h, hp_hublog_t *log, const char *path) {
    memset(h, 0, sizeof *h);
    h->ino = -1;
    h->log = log;
    if (!log || !path || snprintf(h->path, sizeof h->path, "%s", path) >= (int)sizeof h->path) return;
    size_t pl = strlen(h->path);
    if (pl > 4 && !strcmp(h->path + pl - 4, ".log")) snprintf(h->offpath, sizeof h->offpath, "%.*s.off", (int)(pl - 4), h->path);
    load_off(h);
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
