#define _DEFAULT_SOURCE
#include "ota_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tweetnacl.h"

#ifndef OTA_PUBKEY_HEADER
#define OTA_PUBKEY_HEADER "ota_pubkey_dev.h"
#endif
#include OTA_PUBKEY_HEADER

#if OTA_PUBKEY_IS_DEV && defined(HP_RELEASE)
#error "the OTA dev/test public key is baked into a release build: set HP_OTA_PUBKEY_HEADER"
#endif

static const uint8_t g_pubkey[32] = OTA_PUBKEY_INIT;

const uint8_t *ota_pubkey(void) { return g_pubkey; }
int ota_pubkey_is_dev(void) { return OTA_PUBKEY_IS_DEV; }

const char *ota_strerror(int rc) {
    switch (rc) {
    case OTA_OK: return "ok";
    case OTA_E_IO: return "cannot read image";
    case OTA_E_TRUNCATED: return "image truncated";
    case OTA_E_FORMAT: return "bad image header";
    case OTA_E_TOO_BIG: return "payload too large";
    case OTA_E_SIZE: return "file size does not match header";
    case OTA_E_SIG: return "bad signature";
    case OTA_E_TARGET: return "image is for another device";
    case OTA_E_DOWNGRADE: return "downgrade refused";
    case OTA_E_DIGEST: return "payload digest mismatch";
    default: return "unknown error";
    }
}

/* TweetNaCl key generation needs randombytes. ota_keypair_from_seed() feeds it the seed. */
static const uint8_t *g_seed;
void randombytes(unsigned char *buf, unsigned long long len) {
    if (g_seed && len == 32) {
        memcpy(buf, g_seed, 32);
        return;
    }
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned long long got = 0;
    if (fd >= 0) {
        while (got < len) {
            ssize_t n = read(fd, buf + got, (size_t)(len - got));
            if (n <= 0) break;
            got += (unsigned long long)n;
        }
        close(fd);
    }
    if (got < len) abort(); /* never hand out predictable bytes */
}

void ota_keypair_from_seed(const uint8_t seed[32], uint8_t pk[32], uint8_t sk[64]) {
    g_seed = seed; /* host tool / tests only: not thread-safe */
    crypto_sign_keypair(pk, sk);
    g_seed = NULL;
}

/* ---- streaming SHA-512 ---- */
void ota_sha512_init(ota_sha512_t *c) {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                   0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                   0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) c->h[i * 8 + j] = (uint8_t)(iv[i] >> (56 - 8 * j));
    c->used = 0;
    c->total = 0;
}

void ota_sha512_update(ota_sha512_t *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->total += n;
    if (c->used) {
        size_t take = 128 - c->used < n ? 128 - c->used : n;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used < 128) return;
        crypto_hashblocks(c->h, c->buf, 128);
        c->used = 0;
    }
    size_t whole = n & ~(size_t)127;
    if (whole) crypto_hashblocks(c->h, p, whole);
    memcpy(c->buf, p + whole, n - whole);
    c->used = n - whole;
}

void ota_sha512_final(ota_sha512_t *c, uint8_t out[64]) {
    uint8_t pad[256] = {0};
    size_t r = c->used, total = r < 112 ? 128 : 256;
    memcpy(pad, c->buf, r);
    pad[r] = 0x80;
    uint64_t bits = c->total << 3;
    for (int i = 0; i < 8; i++) pad[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    crypto_hashblocks(c->h, pad, total);
    memcpy(out, c->h, 64);
}

/* ---- header ---- */
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int ota_header_pack(uint8_t out[OTA_HDR_BYTES], int target, uint32_t build_id, const uint8_t *payload,
                    size_t len, const uint8_t sk[64]) {
    if (len > OTA_MAX_PAYLOAD || (target != OTA_TARGET_HUB && target != OTA_TARGET_REMOTE)) return OTA_E_FORMAT;
    memset(out, 0, OTA_HDR_BYTES);
    memcpy(out, OTA_MAGIC, 4);
    out[4] = OTA_FORMAT;
    out[5] = (uint8_t)target;
    put32(out + 8, build_id);
    put32(out + 12, (uint32_t)len);
    ota_sha512_t c;
    ota_sha512_init(&c);
    ota_sha512_update(&c, payload, len);
    ota_sha512_final(&c, out + 16);
    uint8_t sm[OTA_SIGNED_BYTES + 64];
    unsigned long long smlen = 0;
    crypto_sign(sm, &smlen, out, OTA_SIGNED_BYTES, sk);
    memcpy(out + OTA_SIGNED_BYTES, sm, 64);
    return OTA_OK;
}

static int verify_stream(FILE *f, const uint8_t pubkey[32], int want_target, uint32_t min_build, ota_hdr_t *out) {
    uint8_t h[OTA_HDR_BYTES];
    if (fread(h, 1, OTA_HDR_BYTES, f) != OTA_HDR_BYTES) return ferror(f) ? OTA_E_IO : OTA_E_TRUNCATED;
    uint32_t len = get32(h + 12);
    if (memcmp(h, OTA_MAGIC, 4) != 0 || h[4] != OTA_FORMAT || (h[5] != OTA_TARGET_HUB && h[5] != OTA_TARGET_REMOTE) ||
        h[6] || h[7])
        return OTA_E_FORMAT;
    if (len > OTA_MAX_PAYLOAD) return OTA_E_TOO_BIG;
    uint8_t sm[OTA_HDR_BYTES], m[OTA_HDR_BYTES];
    unsigned long long mlen = 0;
    memcpy(sm, h + OTA_SIGNED_BYTES, 64); /* signature || message */
    memcpy(sm + 64, h, OTA_SIGNED_BYTES);
    if (crypto_sign_open(m, &mlen, sm, OTA_HDR_BYTES, pubkey) != 0 || mlen != OTA_SIGNED_BYTES) return OTA_E_SIG;
    uint32_t build = get32(h + 8);
    if (h[5] != want_target) return OTA_E_TARGET;
    if (build < min_build) return OTA_E_DOWNGRADE;
    if (fseek(f, 0, SEEK_END) != 0) return OTA_E_IO;
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz != (unsigned long)OTA_HDR_BYTES + len) return OTA_E_SIZE;
    if (fseek(f, OTA_HDR_BYTES, SEEK_SET) != 0) return OTA_E_IO;
    ota_sha512_t c;
    ota_sha512_init(&c);
    uint8_t buf[4096];
    uint32_t left = len;
    while (left) {
        size_t n = fread(buf, 1, left < sizeof buf ? left : sizeof buf, f);
        if (n == 0) return ferror(f) ? OTA_E_IO : OTA_E_SIZE;
        ota_sha512_update(&c, buf, n);
        left -= (uint32_t)n;
    }
    uint8_t dig[64];
    ota_sha512_final(&c, dig);
    if (memcmp(dig, h + 16, 64) != 0) return OTA_E_DIGEST;
    if (out) {
        out->target = h[5];
        out->build_id = build;
        out->payload_len = len;
        memcpy(out->sha512, dig, 64);
    }
    return OTA_OK;
}

int ota_image_verify_file_key(const char *path, const uint8_t pubkey[32], int want_target, uint32_t min_build,
                              ota_hdr_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return OTA_E_IO;
    int rc = verify_stream(f, pubkey, want_target, min_build, out);
    fclose(f);
    return rc;
}

int ota_image_verify_file(const char *path, int want_target, uint32_t min_build, ota_hdr_t *out) {
    return ota_image_verify_file_key(path, g_pubkey, want_target, min_build, out);
}
