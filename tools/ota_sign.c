#define _DEFAULT_SOURCE
/* OTA image signing tool (host side). Release images are signed by CI (hub-firmware.yml) with the release key held as the
 * OTA_SIGNING_KEY secret of the `release` environment (v* tags only); bench images are signed by hand with --dev or a local key.
 *
 *   ota_sign keygen [-k KEY]                         new key; refuses to overwrite
 *   ota_sign pubkey [-k KEY]                         public key, 64 hex characters
 *   ota_sign pubheader [-k KEY | --dev]              C header to build firmware with
 *                                                    (-DHP_OTA_PUBKEY_HEADER=<file>)
 *   ota_sign sign -t hub|remote -b BUILD [-k KEY] PAYLOAD OUT
 *   ota_sign verify [-t hub|remote] [-m MINBUILD] [-k KEY | -p PUBHEX | --dev] IMAGE
 *
 * KEY is a file holding the 32-byte seed as 64 hex characters, default
 * $HOME/.config/ha-harmony/ota_ed25519 (mode 0600, outside the repository, never committed).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ota_image.h"

static const uint8_t DEV_SEED[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static int die(const char *msg) {
    fprintf(stderr, "ota_sign: %s\n", msg);
    return 1;
}

static int default_key_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    return snprintf(out, n, "%s/.config/ha-harmony/ota_ed25519", home) < (int)n ? 0 : -1;
}

static int unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        char t[3] = {s[2 * i], s[2 * i + 1], 0};
        char *end;
        if (!t[0] || !t[1]) return -1;
        v = (unsigned)strtoul(t, &end, 16);
        if (*end) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int load_seed(const char *path, uint8_t seed[32]) {
    FILE *f = fopen(path, "r");
    char line[256] = {0};
    if (!f) return -1;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    line[strcspn(line, "\r\n ")] = 0;
    return strlen(line) == 64 ? unhex(line, seed, 32) : -1;
}

static void hex(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0, r;
    uint8_t *buf = malloc(cap);
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            uint8_t *g = realloc(buf, cap *= 2);
            if (!g) {
                free(buf);
                buf = NULL;
            } else {
                buf = g;
            }
        }
    }
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) return die("usage: keygen | pubkey | pubheader | sign | verify (see tools/ota_sign.c)");
    const char *cmd = argv[1], *key = NULL, *pubhex = NULL;
    int target = 0, dev = 0;
    unsigned long build = 0, minb = 0;
    int have_build = 0, pos_n = 0;
    const char *pos[2] = {0, 0};
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pubhex = argv[++i];
        else if (!strcmp(argv[i], "--dev")) dev = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) { build = strtoul(argv[++i], NULL, 10); have_build = 1; }
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) minb = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            const char *t = argv[++i];
            target = !strcmp(t, "hub") ? OTA_TARGET_HUB : !strcmp(t, "remote") ? OTA_TARGET_REMOTE : -1;
            if (target < 0) return die("target must be hub or remote");
        } else if (argv[i][0] != '-' && pos_n < 2) pos[pos_n++] = argv[i];
        else return die("bad argument");
    }
    char kpath[512];
    if (!key && default_key_path(kpath, sizeof kpath) == 0) key = kpath;
    uint8_t seed[32], pk[32], sk[64];

    if (!strcmp(cmd, "keygen")) {
        if (!key) return die("no key path (set HOME or pass -k)");
        if (access(key, F_OK) == 0) return die("key file exists, refusing to overwrite");
        FILE *rnd = fopen("/dev/urandom", "rb");
        if (!rnd || fread(seed, 1, 32, rnd) != 32) return die("cannot read /dev/urandom");
        fclose(rnd);
        char dir[512];
        snprintf(dir, sizeof dir, "%s", key);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            for (char *p = dir + 1; *p; p++)
                if (*p == '/') { *p = 0; mkdir(dir, 0700); *p = '/'; }
            mkdir(dir, 0700);
        }
        mode_t old = umask(0077);
        FILE *f = fopen(key, "w");
        umask(old);
        if (!f) return die(strerror(errno));
        for (int i = 0; i < 32; i++) fprintf(f, "%02x", seed[i]);
        fprintf(f, "\n");
        fclose(f);
        ota_keypair_from_seed(seed, pk, sk);
        printf("private key written to %s (keep it private, never commit it)\npublic key: ", key);
        hex(pk, 32);
        printf("\n");
        return 0;
    }

    if (dev) memcpy(seed, DEV_SEED, 32);
    else if (!(key && load_seed(key, seed) == 0)) {
        if (!strcmp(cmd, "verify") && pubhex) memset(seed, 0, 32);
        else return die("cannot read private key (run keygen, or pass -k)");
    }
    ota_keypair_from_seed(seed, pk, sk);

    if (!strcmp(cmd, "pubkey")) {
        hex(pk, 32);
        printf("\n");
        return 0;
    }
    if (!strcmp(cmd, "pubheader")) {
        printf("/* Generated by ota_sign pubheader. Public key only. */\n#ifndef OTA_PUBKEY_GEN_H\n"
               "#define OTA_PUBKEY_GEN_H\n#define OTA_PUBKEY_IS_DEV %d\n#define OTA_PUBKEY_INIT \\\n    { ", dev);
        for (int i = 0; i < 32; i++) printf("0x%02x%s", pk[i], i < 31 ? ", " : " }\n");
        printf("#endif\n");
        return 0;
    }
    if (!strcmp(cmd, "sign")) {
        if (!target || pos_n != 2) return die("sign needs -t, [-b], PAYLOAD and OUT");
        size_t len;
        uint8_t *payload = slurp(pos[0], &len);
        if (!payload && len == 0 && errno) return die("cannot read payload");
        if (!have_build && payload && target == OTA_TARGET_HUB) { /* the id the hub binary was built with */
            static const char tag[] = "HPBUILDID=";
            for (size_t i = 0; i + sizeof tag < len && !have_build; i++) {
                if (memcmp(payload + i, tag, sizeof tag - 1) != 0) continue;
                char *end = NULL;
                unsigned long v = strtoul((const char *)payload + i + sizeof tag - 1, &end, 10);
                if (end && *end == ';' && v > 0) { build = v; have_build = 1; }
            }
        }
        if (!have_build) return die("sign needs -b BUILD (a hub binary built here carries its own id)");
        uint8_t h[OTA_HDR_BYTES];
        if (!payload || ota_header_pack(h, target, (uint32_t)build, payload, len, sk) != OTA_OK)
            return die("payload too large or unreadable");
        FILE *o = fopen(pos[1], "wb");
        if (!o || fwrite(h, 1, sizeof h, o) != sizeof h || fwrite(payload, 1, len, o) != len || fclose(o) != 0)
            return die("cannot write output");
        free(payload);
        printf("signed %s build %lu (%zu bytes)\n", target == OTA_TARGET_HUB ? "hub" : "remote", build, len);
        return 0;
    }
    if (!strcmp(cmd, "verify")) {
        if (pos_n != 1) return die("verify needs IMAGE");
        if (pubhex && unhex(pubhex, pk, 32) != 0) return die("bad -p public key");
        ota_hdr_t hd;
        /* -t absent: accept either target by trying both */
        int rc = target ? ota_image_verify_file_key(pos[0], pk, target, (uint32_t)minb, &hd)
                        : ota_image_verify_file_key(pos[0], pk, OTA_TARGET_HUB, (uint32_t)minb, &hd);
        if (!target && rc == OTA_E_TARGET) rc = ota_image_verify_file_key(pos[0], pk, OTA_TARGET_REMOTE, (uint32_t)minb, &hd);
        if (rc != OTA_OK) {
            fprintf(stderr, "ota_sign: %s\n", ota_strerror(rc));
            return 1;
        }
        printf("ok target=%s build=%u payload=%u\n", hd.target == OTA_TARGET_HUB ? "hub" : "remote",
               (unsigned)hd.build_id, (unsigned)hd.payload_len);
        return 0;
    }
    return die("unknown command");
}
