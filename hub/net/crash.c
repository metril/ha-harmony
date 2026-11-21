#define _GNU_SOURCE
#include "hub/net/crash.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_crumb[HP_CRUMBS][HP_CRUMB_LEN];
static _Atomic unsigned g_next;
static char g_hook[256];

void hp_crumb_raw(const char *s, size_t n) {
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n >= HP_CRUMB_LEN) n = HP_CRUMB_LEN - 1;
    char *d = g_crumb[atomic_fetch_add(&g_next, 1) % HP_CRUMBS];
    d[0] = 0; /* a reader racing this slot sees an empty line, never a half-written one's tail */
    memcpy(d, s, n);
    d[n] = 0;
}

void hp_crumb(const char *fmt, ...) {
    char b[HP_CRUMB_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) hp_crumb_raw(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static size_t put(char *o, size_t cap, size_t at, const char *s) {
    while (*s && at + 1 < cap) o[at++] = *s++;
    o[at] = 0;
    return at;
}

static size_t put_num(char *o, size_t cap, size_t at, unsigned v) {
    char t[12];
    int k = 0;
    do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11);
    while (k) {
        if (at + 1 < cap) o[at++] = t[--k];
        else k--;
    }
    o[at] = 0;
    return at;
}

/* Async-signal-safe: no stdio, no malloc. One line, crumbs oldest first, joined by " | ". */
size_t hp_crash_format(int sig, char *out, size_t cap) {
    const char *nm = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : sig == SIGBUS ? "SIGBUS"
                   : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "signal";
    size_t at = put(out, cap, 0, "hub: FATAL ");
    at = put(out, cap, at, nm);
    at = put(out, cap, at, " (");
    at = put_num(out, cap, at, (unsigned)sig);
    at = put(out, cap, at, "), last:");
    unsigned nx = atomic_load(&g_next);
    for (unsigned i = 0; i < HP_CRUMBS; i++) {
        const char *c = g_crumb[(nx + i) % HP_CRUMBS];
        if (!*c) continue;
        at = put(out, cap, at, " | ");
        at = put(out, cap, at, c);
    }
    if (at + 1 < cap) out[at++] = '\n';
    out[at < cap ? at : cap - 1] = 0;
    return at;
}

static void wr(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void on_fatal(int sig) {
    char b[HP_CRUMBS * (HP_CRUMB_LEN + 3) + 64];
    size_t n = hp_crash_format(sig, b, sizeof b);
    wr(STDERR_FILENO, b, n);
    if (g_hook[0]) { /* the boot hook's tmpfs log: the next daemon ships it. O_APPEND, so it lands after the hook's own lines */
        int fd = open(g_hook, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd >= 0) {
            wr(fd, "[daemon] ", 9);
            wr(fd, b + 5, n - 5); /* without the "hub: " prefix */
            close(fd);
        }
    }
    raise(sig); /* SA_RESETHAND restored the default action: die with the real signal */
}

void hp_crash_install(const char *hook_log) {
    g_hook[0] = 0;
    if (hook_log && strlen(hook_log) < sizeof g_hook) snprintf(g_hook, sizeof g_hook, "%s", hook_log);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    static const int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
