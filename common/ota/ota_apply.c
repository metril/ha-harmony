#define _DEFAULT_SOURCE
#include "ota_apply.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void (*ota_apply_test_hook)(void); /* tests only: runs between verify and copy */

/* Copy the payload from the already-verified open file, hashing the bytes actually written;
 * the digest must equal the signed one, so a staged file changed after verify is refused. */
static int copy_payload(FILE *in, const char *dst, const ota_hdr_t *hdr, mode_t mode) {
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return OTA_E_IO;
    if (fseek(in, OTA_HDR_BYTES, SEEK_SET) != 0) {
        close(fd);
        return OTA_E_IO;
    }
    ota_sha512_t c;
    ota_sha512_init(&c);
    unsigned char buf[4096];
    uint32_t len = hdr->payload_len;
    int rc = OTA_OK;
    while (len && rc == OTA_OK) {
        size_t n = fread(buf, 1, len < sizeof buf ? len : sizeof buf, in);
        if (n == 0) {
            rc = OTA_E_IO;
            break;
        }
        ota_sha512_update(&c, buf, n);
        for (size_t off = 0; off < n;) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w <= 0) {
                rc = OTA_E_IO;
                break;
            }
            off += (size_t)w;
        }
        len -= (uint32_t)n;
    }
    unsigned char dig[64];
    ota_sha512_final(&c, dig);
    if (rc == OTA_OK && memcmp(dig, hdr->sha512, 64) != 0) rc = OTA_E_DIGEST;
    if (rc == OTA_OK && fsync(fd) != 0) rc = OTA_E_IO;
    if (close(fd) != 0 && rc == OTA_OK) rc = OTA_E_IO;
    return rc;
}

int ota_apply_staged_cb(const char *staged, const char *target, const char *prev_path, uint32_t min_build,
                        const uint8_t pubkey[32], uint32_t *build_out, void (*before_swap)(void *), void *ctx) {
    FILE *in = fopen(staged, "rb");
    if (!in) return OTA_E_IO;
    ota_hdr_t hdr;
    int rc = ota_image_verify_stream(in, pubkey, OTA_TARGET_HUB, min_build, &hdr);
    if (rc != OTA_OK) {
        fclose(in);
        return rc;
    }
    if (ota_apply_test_hook) ota_apply_test_hook();
    char neu[512], tmp[512];
    if (snprintf(neu, sizeof neu, "%s.new", target) >= (int)sizeof neu ||
        snprintf(tmp, sizeof tmp, "%s.lnk", prev_path) >= (int)sizeof tmp) {
        fclose(in);
        return OTA_E_IO;
    }
    rc = copy_payload(in, neu, &hdr, 0755);
    fclose(in);
    if (rc != OTA_OK) {
        unlink(neu);
        return rc;
    }
    unlink(tmp);
    if (link(target, tmp) != 0 || rename(tmp, prev_path) != 0) {
        unlink(tmp);
        unlink(neu);
        return OTA_E_IO;
    }
    if (before_swap) before_swap(ctx);
    if (rename(neu, target) != 0) {
        unlink(neu);
        return OTA_E_IO;
    }
    if (build_out) *build_out = hdr.build_id;
    return OTA_OK;
}

int ota_apply_staged(const char *staged, const char *target, const char *prev_path, uint32_t min_build,
                     const uint8_t pubkey[32], uint32_t *build_out) {
    return ota_apply_staged_cb(staged, target, prev_path, min_build, pubkey, build_out, NULL, NULL);
}

int ota_rollback(const char *target, const char *prev_path) {
    struct stat st;
    if (stat(prev_path, &st) != 0) return -1;
    return rename(prev_path, target) == 0 ? 0 : -1;
}
