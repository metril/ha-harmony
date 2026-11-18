#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Signed OTA image: 144-byte header followed by the payload (little-endian fields).
 *   0   magic "HOTA"
 *   4   format version (1)
 *   5   target (1 = hub, 2 = remote)
 *   6   flags (u16, must be 0)
 *   8   build id (u32, monotonic)
 *   12  payload length (u32)
 *   16  SHA-512 of the payload (64)
 *   80  Ed25519 signature over bytes 0..79 (64)
 * File size must equal 144 + payload length. The signature covers the digest, so a staged
 * file is verified by one pass over the payload with O(1) memory. */
#define OTA_MAGIC "HOTA"
#define OTA_FORMAT 1
#define OTA_HDR_BYTES 144
#define OTA_SIGNED_BYTES 80
#define OTA_MAX_PAYLOAD (32u * 1024u * 1024u)

enum { OTA_TARGET_HUB = 1, OTA_TARGET_REMOTE = 2 };

enum {
    OTA_OK = 0,
    OTA_E_IO = -1,
    OTA_E_TRUNCATED = -2,
    OTA_E_FORMAT = -3,   /* magic, version, flags or target value */
    OTA_E_TOO_BIG = -4,  /* payload length above OTA_MAX_PAYLOAD */
    OTA_E_SIZE = -5,     /* file size != header + payload length */
    OTA_E_SIG = -6,
    OTA_E_TARGET = -7,   /* image is for the other device */
    OTA_E_DOWNGRADE = -8,
    OTA_E_DIGEST = -9
};

typedef struct {
    uint8_t target;
    uint32_t build_id;
    uint32_t payload_len;
    uint8_t sha512[64];
} ota_hdr_t;

const char *ota_strerror(int rc);

/* Streaming SHA-512 (built on TweetNaCl's block function). */
typedef struct {
    uint8_t h[64];
    uint8_t buf[128];
    size_t used;
    uint64_t total;
} ota_sha512_t;
void ota_sha512_init(ota_sha512_t *c);
void ota_sha512_update(ota_sha512_t *c, const void *data, size_t n);
void ota_sha512_final(ota_sha512_t *c, uint8_t out[64]);

/* Verify a staged image file against pubkey. want_target: OTA_TARGET_*. Images with
 * build_id < min_build are refused as downgrades (pass 0 to disable). On success fills *out. */
int ota_image_verify_file_key(const char *path, const uint8_t pubkey[32], int want_target,
                              uint32_t min_build, ota_hdr_t *out);
/* Same on an already-open file (seeks it); callers that go on to read the payload use this so
 * verification and use share one file description. */
int ota_image_verify_stream(FILE *f, const uint8_t pubkey[32], int want_target, uint32_t min_build, ota_hdr_t *out);
/* Same with the public key compiled into this build. */
int ota_image_verify_file(const char *path, int want_target, uint32_t min_build, ota_hdr_t *out);

/* Compiled-in key: 1 when it is the dev/test key (never acceptable in a release build). */
int ota_pubkey_is_dev(void);
const uint8_t *ota_pubkey(void);

/* Signing side (host tool and tests). sk is the 64-byte TweetNaCl secret key (seed||pk). */
void ota_keypair_from_seed(const uint8_t seed[32], uint8_t pk[32], uint8_t sk[64]);
int ota_header_pack(uint8_t out[OTA_HDR_BYTES], int target, uint32_t build_id, const uint8_t *payload,
                    size_t len, const uint8_t sk[64]);

#endif
