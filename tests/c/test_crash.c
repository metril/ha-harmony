#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/net/crash.h"
#include "hub/net/hublog.h"

static void slurp(const char *p, char *b, size_t cap) {
    int fd = open(p, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, b, cap - 1) : -1;
    b[n < 0 ? 0 : n] = 0;
    if (fd >= 0) close(fd);
}

int main(void) {
    char b[1024];
    /* Format: the signal, then the breadcrumbs oldest first; older ones fall out of the ring. */
    for (int i = 0; i < 12; i++) hp_crumb("step %d", i);
    size_t n = hp_crash_format(SIGSEGV, b, sizeof b);
    CT_CHECK(n == strlen(b) && n > 0 && b[n - 1] == '\n');
    CT_CHECK(strstr(b, "FATAL SIGSEGV (11)"));
    CT_CHECK(!strstr(b, "step 3") && strstr(b, "step 4") && strstr(b, "step 11"));
    CT_CHECK(strstr(b, "step 4") < strstr(b, "step 11"));
    hp_crash_format(SIGABRT, b, 24); /* a tiny buffer is cut, never overrun */
    CT_CHECK(strlen(b) < 24);

    /* hp_log lines and commands are crumbs too (a long one is cut). */
    hp_log("hub: ir send failed rc=%d", -5);
    char longl[400];
    memset(longl, 'x', sizeof longl - 1);
    longl[sizeof longl - 1] = 0;
    hp_crumb("%s", longl);
    hp_crash_format(SIGABRT, b, sizeof b);
    CT_CHECK(strstr(b, "ir send failed rc=-5") && strstr(b, "SIGABRT"));
    CT_CHECK(strlen(b) < 900);

    /* A real crash in a child: the line reaches the log fd and the hook log, and the death is still by the signal. */
    char hook[] = "/tmp/hp-crash-hookXXXXXX", err[] = "/tmp/hp-crash-errXXXXXX";
    int hfd = mkstemp(hook), efd = mkstemp(err);
    CT_CHECK(hfd >= 0 && efd >= 0);
    CT_CHECK(write(hfd, "[up 5s] earlier hook line\n", 26) == 26);
    static const int sigs[] = {SIGSEGV, SIGABRT};
    for (int i = 0; i < 2; i++) {
        pid_t p = fork();
        CT_CHECK(p >= 0);
        if (!p) {
            dup2(efd, STDERR_FILENO);
            hp_crash_install(hook);
            hp_crumb("cmd hub ir_send");
            hp_crumb("ir ports=3 n=68 hz=38000");
            if (i == 0) *(volatile int *)0 = 1; /* the real thing, not raise() */
            abort();
        }
        int st = 0;
        CT_CHECK(waitpid(p, &st, 0) == p);
        CT_CHECK(WIFSIGNALED(st) && WTERMSIG(st) == sigs[i]);
    }
    slurp(hook, b, sizeof b);
    CT_CHECK(!strncmp(b, "[up 5s] earlier hook line\n[daemon] FATAL SIGSEGV (11)", 51));
    CT_CHECK(strstr(b, "[daemon] FATAL SIGABRT (6)"));
    CT_CHECK(strstr(b, "| ir ports=3 n=68 hz=38000\n"));
    slurp(err, b, sizeof b);
    CT_CHECK(strstr(b, "hub: FATAL SIGSEGV (11)") && strstr(b, "cmd hub ir_send"));
    unlink(hook);
    unlink(err);
    return 0;
}
