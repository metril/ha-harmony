#include "bt_leaddr.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool bt_leaddr_valid(const uint8_t a[6]) {
    if ((a[0] & 0xC0) != 0xC0) return false;
    uint8_t any = a[0] & 0x3F, all = a[0] | 0xC0;
    for (int i = 1; i < 6; i++) {
        any |= a[i];
        all &= a[i];
    }
    return any != 0 && all != 0xFF;
}

static bool urandom_fill(uint8_t *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

bool bt_leaddr_generate(uint8_t a[6], bool (*rnd)(uint8_t *, size_t)) {
    if (!rnd) rnd = urandom_fill;
    for (int tries = 0; tries < 8; tries++) {
        if (!rnd(a, 6)) return false;
        a[0] |= 0xC0;
        if (bt_leaddr_valid(a)) return true;
    }
    return false;
}

static bool store(const char *path, const uint8_t a[6]) {
    char tmp[300];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, a, 6) == 6 && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

int bt_leaddr_load_or_create(const char *path, uint8_t a[6], bool (*rnd)(uint8_t *, size_t)) {
    uint8_t b[7];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT) return -1; /* cannot tell: keep the file, use the public address this run */
    if (fd >= 0) {
        ssize_t n = read(fd, b, sizeof b);
        close(fd);
        if (n < 0) return -1;
        if (n == 6 && bt_leaddr_valid(b)) {
            memcpy(a, b, 6);
            return 0;
        }
        /* read completely and invalid: replaced below */
    }
    uint8_t n6[6];
    if (!bt_leaddr_generate(n6, rnd) || !store(path, n6)) return -1;
    memcpy(a, n6, 6);
    return 1;
}
