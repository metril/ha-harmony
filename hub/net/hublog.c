#define _GNU_SOURCE
#include "hub/net/hublog.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hub/hp_errors.h"

static uint32_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000)) : 0;
}

int hp_hublog_init(hp_hublog_t *l, hp_log_emit_fn emit, void *ctx) {
    memset(l, 0, sizeof *l);
    l->wake[0] = l->wake[1] = l->orig_fd = l->pipe_r = l->pipe_w = -1;
    if (pthread_mutex_init(&l->mu, NULL) != 0 || pipe2(l->wake, O_NONBLOCK | O_CLOEXEC) < 0) return -HP_E_INVAL;
    l->emit = emit;
    l->ctx = ctx;
    return 0;
}

int hp_hublog_wake_fd(const hp_hublog_t *l) { return l->wake[0]; }

const char *hp_hublog_level(const char *m) {
    static const char *const w[] = {"cannot", "failed", "error", "giving up", "dropping", "refus", "unusable", NULL};
    for (int i = 0; w[i]; i++)
        if (strstr(m, w[i])) return "warning";
    return "info";
}

static void push(hp_hublog_t *l, const char *s, size_t n) { /* mu held */
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    if (!n) return;
    if (l->count == HP_LOG_RING) {
        l->head = (l->head + 1) % HP_LOG_RING;
        l->count--;
        l->dropped++;
    }
    hp_logline_t *e = &l->ring[(l->head + l->count++) % HP_LOG_RING];
    memcpy(e->msg, s, n);
    e->msg[n] = 0;
    snprintf(e->level, sizeof e->level, "%s", hp_hublog_level(e->msg));
    e->ts = now_ms();
    e->dropped = 0;
}

void hp_hublog_write(hp_hublog_t *l, const char *buf, size_t n) {
    pthread_mutex_lock(&l->mu);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            if (!l->over || l->plen) { push(l, l->part, l->plen); any = true; }
            l->plen = 0;
            l->over = false;
        } else if (l->over) {
            continue;
        } else if (l->plen < sizeof l->part - 1) {
            l->part[l->plen++] = (unsigned char)c < 0x20 && c != '\t' ? ' ' : c;
        } else {
            l->over = true;
        }
    }
    bool kick = any && !l->woken;
    if (kick) l->woken = true;
    pthread_mutex_unlock(&l->mu);
    if (kick) { char b = 1; (void)!write(l->wake[1], &b, 1); }
}

void hp_hublog_service(hp_hublog_t *l) {
    char junk[16];
    pthread_mutex_lock(&l->mu);
    l->woken = false;
    pthread_mutex_unlock(&l->mu);
    while (read(l->wake[0], junk, sizeof junk) > 0) {}
    for (;;) {
        hp_logline_t e;
        pthread_mutex_lock(&l->mu);
        if (!l->count) { pthread_mutex_unlock(&l->mu); return; }
        e = l->ring[l->head];
        l->head = (l->head + 1) % HP_LOG_RING;
        l->count--;
        e.dropped = l->dropped;
        l->dropped = 0;
        pthread_mutex_unlock(&l->mu);
        if (l->emit && l->emit(l->ctx, &e)) continue;
        pthread_mutex_lock(&l->mu); /* nobody to tell: put it back at the front (or count it lost if the ring refilled) */
        if (l->count < HP_LOG_RING) {
            l->head = (l->head + HP_LOG_RING - 1) % HP_LOG_RING;
            l->ring[l->head] = e;
            l->count++;
            l->dropped += e.dropped;
        } else {
            l->dropped += e.dropped + 1;
        }
        pthread_mutex_unlock(&l->mu);
        return;
    }
}

static void *reader(void *arg) {
    hp_hublog_t *l = arg;
    char buf[512];
    for (;;) {
        ssize_t r = read(l->pipe_r, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        pthread_mutex_lock(&l->mu);
        bool q = l->quit;
        pthread_mutex_unlock(&l->mu);
        if (q && buf[r - 1] == 0) r--; /* hp_hublog_release's sentinel: not a log byte */
        for (ssize_t off = 0; off < r;) { /* tee first: the log file must not wait on anything else */
            ssize_t w = write(l->orig_fd, buf + off, (size_t)(r - off));
            if (w <= 0) break;
            off += w;
        }
        if (r > 0) hp_hublog_write(l, buf, (size_t)r);
        if (q) break;
    }
    return NULL;
}

int hp_hublog_capture(hp_hublog_t *l) {
    int p[2];
    if (l->running || pipe2(p, O_CLOEXEC) < 0) return -HP_E_INVAL;
    int keep = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 10);
    if (keep < 0) { close(p[0]); close(p[1]); return -HP_E_INVAL; }
    l->orig_fd = keep;
    l->pipe_r = p[0];
    l->pipe_w = p[1];
    if (pthread_create(&l->th, NULL, reader, l) != 0) {
        close(p[0]); close(p[1]); close(keep);
        return -HP_E_INVAL;
    }
    l->running = true;
    dup2(p[1], STDERR_FILENO); /* dup2 clears CLOEXEC: the BT helper child keeps logging into the same stream */
    return 0;
}

void hp_hublog_release(hp_hublog_t *l) {
    if (!l->running) return;
    dup2(l->orig_fd, STDERR_FILENO);
    pthread_mutex_lock(&l->mu);
    l->quit = true;
    pthread_mutex_unlock(&l->mu);
    (void)!write(l->pipe_w, "", 1); /* wakes the reader (children may still hold the write end, so no EOF) */
    pthread_join(l->th, NULL);
    close(l->pipe_r);
    close(l->pipe_w);
    close(l->orig_fd);
    l->running = false;
}
