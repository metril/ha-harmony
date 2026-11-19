#include "bt_bonds.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* BTstack POSIX TLV file: 8-byte header starting with "BTstack", then records
 * tag u32 BE, len u32 BE, value (len 0 = tag deleted). */
#define HDR 8
#define MAX_VALUE 2048

static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* Reads PATH into a compacted image; NULL when the file is missing or its header is bad */
static uint8_t *load(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *in = sz >= HDR ? malloc((size_t)sz) : NULL;
    if (!in || fread(in, 1, (size_t)sz, f) != (size_t)sz || memcmp(in, "BTstack", 7) != 0) {
        free(in);
        fclose(f);
        return NULL;
    }
    fclose(f);
    uint8_t *out = malloc((size_t)sz + HDR);
    if (!out) { free(in); return NULL; }
    memcpy(out, in, HDR);
    size_t on = HDR, pos = HDR;
    /* walk the valid records, then keep each one that no later record overrides */
    while (pos + 8 <= (size_t)sz) {
        uint32_t len = rd32(in + pos + 4);
        if (len > MAX_VALUE || pos + 8 + len > (size_t)sz) break; /* bad or truncated tail: dropped */
        pos += 8 + len;
    }
    size_t end = pos;
    for (pos = HDR; pos < end;) {
        uint32_t tag = rd32(in + pos), len = rd32(in + pos + 4);
        size_t next = pos + 8 + len;
        int later = 0;
        for (size_t q = next; q < end && !later;) {
            uint32_t l2 = rd32(in + q + 4);
            if (rd32(in + q) == tag) later = 1;
            q += 8 + l2;
        }
        if (!later && len > 0) {
            memcpy(out + on, in + pos, 8 + len);
            on += 8 + len;
        }
        pos = next;
    }
    free(in);
    *out_len = on;
    return out;
}

static void sync_dir(const char *path) {
    char d[300];
    snprintf(d, sizeof d, "%s", path);
    char *sl = strrchr(d, '/');
    if (!sl) return;
    *sl = 0;
    int fd = open(*d ? d : "/", O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

int bt_bonds_prepare(const char *path) {
    char bak[300], tmp[300];
    snprintf(bak, sizeof bak, "%s.bak", path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    size_t n = 0;
    int from_bak = 0;
    uint8_t *img = load(path, &n);
    if (!img) {
        img = load(bak, &n);
        from_bak = 1;
    }
    if (!img) return 0;
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(img, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) fclose(f);
    free(img);
    if (!ok) { unlink(tmp); return -1; }
    if (!from_bak) rename(path, bak); /* the previous good file stays as .bak */
    if (rename(tmp, path) != 0) return -1;
    sync_dir(path);
    return 1;
}
