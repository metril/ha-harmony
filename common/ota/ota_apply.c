#define _DEFAULT_SOURCE
#include "ota_apply.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int copy_payload(const char *staged, const char *dst, uint32_t len, mode_t mode) {
    FILE *in = fopen(staged, "rb");
    if (!in) return -1;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || fseek(in, OTA_HDR_BYTES, SEEK_SET) != 0) {
        if (fd >= 0) close(fd);
        fclose(in);
        return -1;
    }
    unsigned char buf[4096];
    int rc = 0;
    while (len && rc == 0) {
        size_t n = fread(buf, 1, len < sizeof buf ? len : sizeof buf, in);
        if (n == 0) {
            rc = -1;
            break;
        }
        for (size_t off = 0; off < n;) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w <= 0) {
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
        len -= (uint32_t)n;
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    fclose(in);
    return rc;
}

int ota_apply_staged(const char *staged, const char *target, const char *prev_path, uint32_t min_build,
                     const uint8_t pubkey[32], uint32_t *build_out) {
    ota_hdr_t hdr;
    int rc = ota_image_verify_file_key(staged, pubkey, OTA_TARGET_HUB, min_build, &hdr);
    if (rc != OTA_OK) return rc;
    char neu[512], tmp[512];
    if (snprintf(neu, sizeof neu, "%s.new", target) >= (int)sizeof neu ||
        snprintf(tmp, sizeof tmp, "%s.lnk", prev_path) >= (int)sizeof tmp)
        return OTA_E_IO;
    if (copy_payload(staged, neu, hdr.payload_len, 0755) != 0) {
        unlink(neu);
        return OTA_E_IO;
    }
    unlink(tmp);
    if (link(target, tmp) != 0 || rename(tmp, prev_path) != 0) {
        unlink(tmp);
        unlink(neu);
        return OTA_E_IO;
    }
    if (rename(neu, target) != 0) {
        unlink(neu);
        return OTA_E_IO;
    }
    if (build_out) *build_out = hdr.build_id;
    return OTA_OK;
}

int ota_rollback(const char *target, const char *prev_path) {
    struct stat st;
    if (stat(prev_path, &st) != 0) return -1;
    return rename(prev_path, target) == 0 ? 0 : -1;
}
