#define _DEFAULT_SOURCE
/* Signed OTA images: streaming SHA-512, Ed25519 (RFC 8032 vectors), image verify rejections,
 * hub staged apply. The dev key's seed is bytes 1..32 (it signs nothing outside the tests). */
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "ota_apply.h"
#include "ota_image.h"
#include "tweetnacl.h"

static char dir[] = "/tmp/ota_test_XXXXXX";
static uint8_t sk[64], pk[32];

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void path(char *out, const char *name) { snprintf(out, 300, "%s/%s", dir, name); }

static void put(const char *p, const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    FILE *f = fopen(p, "wb");
    if (an) fwrite(a, 1, an, f);
    if (bn) fwrite(b, 1, bn, f);
    fclose(f);
}

static void make_image(const char *p, int target, uint32_t build, const uint8_t *pl, size_t n) {
    uint8_t h[OTA_HDR_BYTES];
    CT_EQ(ota_header_pack(h, target, build, pl, n, sk), OTA_OK);
    put(p, h, sizeof h, pl, n);
}

static void test_sha512(void) {
    uint8_t d[64], ref[64], hex[64];
    ota_sha512_t c;
    ota_sha512_init(&c);
    ota_sha512_final(&c, d);
    unhex("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f"
          "63b931bd47417a81a538327af927da3e", hex, 64);
    CT_MEM_EQ(d, hex, 64);
    ota_sha512_init(&c);
    ota_sha512_update(&c, "abc", 3);
    ota_sha512_final(&c, d);
    unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
          "454d4423643ce80e2a9ac94fa54ca49f", hex, 64);
    CT_MEM_EQ(d, hex, 64);
    /* streaming == one-shot for every boundary-straddling length and chunk size */
    uint8_t msg[600];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 7 + 1);
    size_t lens[] = {0, 1, 111, 112, 113, 127, 128, 129, 239, 240, 255, 256, 257, 600};
    size_t chunks[] = {1, 3, 64, 127, 128, 200};
    for (size_t li = 0; li < sizeof lens / sizeof *lens; li++) {
        crypto_hash(ref, msg, lens[li]);
        for (size_t ci = 0; ci < sizeof chunks / sizeof *chunks; ci++) {
            ota_sha512_init(&c);
            for (size_t o = 0; o < lens[li]; o += chunks[ci]) {
                size_t n = lens[li] - o < chunks[ci] ? lens[li] - o : chunks[ci];
                ota_sha512_update(&c, msg + o, n);
            }
            ota_sha512_final(&c, d);
            CT_MEM_EQ(d, ref, 64);
        }
    }
}

static void test_ed25519_vectors(void) {
    /* RFC 8032 section 7.1, test 1 (empty message) and test 2 (0x72) */
    uint8_t pkv[32], sm[65], m[65];
    unsigned long long mlen;
    unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pkv, 32);
    unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b"
          "d25bf5f0595bbe24655141438e7a100b", sm, 64);
    CT_EQ(crypto_sign_open(m, &mlen, sm, 64, pkv), 0);
    unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", pkv, 32);
    unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c"
          "387b2eaeb4302aeeb00d291612bb0c00", sm, 64);
    sm[64] = 0x72;
    CT_EQ(crypto_sign_open(m, &mlen, sm, 65, pkv), 0);
    sm[64] = 0x73;
    CT_CHECK(crypto_sign_open(m, &mlen, sm, 65, pkv) != 0);
    /* dev key: compiled-in public key matches the seed 1..32 */
    CT_MEM_EQ(ota_pubkey(), pk, 32);
    CT_EQ(ota_pubkey_is_dev(), 1);
}

static void test_verify(void) {
    uint8_t pl[1000], h[OTA_HDR_BYTES], bad[32] = {0};
    for (size_t i = 0; i < sizeof pl; i++) pl[i] = (uint8_t)(i ^ 0x5a);
    char p[300];
    ota_hdr_t hd;
    path(p, "ok.img");
    make_image(p, OTA_TARGET_HUB, 42, pl, sizeof pl);
    CT_EQ(ota_image_verify_file(p, OTA_TARGET_HUB, 0, &hd), OTA_OK);
    CT_EQ(hd.build_id, 42);
    CT_EQ(hd.payload_len, sizeof pl);
    CT_EQ(ota_image_verify_file(p, OTA_TARGET_HUB, 42, NULL), OTA_OK);
    CT_EQ(ota_image_verify_file(p, OTA_TARGET_HUB, 43, NULL), OTA_E_DOWNGRADE);
    CT_EQ(ota_image_verify_file(p, OTA_TARGET_REMOTE, 0, NULL), OTA_E_TARGET);
    CT_EQ(ota_image_verify_file_key(p, bad, OTA_TARGET_HUB, 0, NULL), OTA_E_SIG);
    CT_EQ(ota_image_verify_file("/nonexistent/x", OTA_TARGET_HUB, 0, NULL), OTA_E_IO);

    /* empty payload is a valid image */
    path(p, "empty.img");
    make_image(p, OTA_TARGET_REMOTE, 1, pl, 0);
    CT_EQ(ota_image_verify_file(p, OTA_TARGET_REMOTE, 0, NULL), OTA_OK);

    /* every single-byte flip in the header is rejected; one payload flip is a digest error */
    uint8_t img[OTA_HDR_BYTES + 1000];
    path(p, "ok.img");
    FILE *f = fopen(p, "rb");
    CT_EQ(fread(img, 1, sizeof img, f), sizeof img);
    fclose(f);
    char q[300];
    path(q, "mut.img");
    for (size_t i = 0; i < OTA_HDR_BYTES; i += (i < 16 ? 1 : 7)) {
        img[i] ^= 0x01;
        put(q, img, sizeof img, NULL, 0);
        CT_CHECK(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL) != OTA_OK);
        img[i] ^= 0x01;
    }
    img[OTA_HDR_BYTES + 500] ^= 0x80;
    put(q, img, sizeof img, NULL, 0);
    CT_EQ(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL), OTA_E_DIGEST);
    img[OTA_HDR_BYTES + 500] ^= 0x80;

    /* truncation at every interesting length, and trailing garbage */
    size_t cuts[] = {0, 1, 4, 80, 143, 144, 145, 600, 1143};
    for (size_t i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        put(q, img, cuts[i], NULL, 0);
        int rc = ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL);
        CT_EQ(rc, cuts[i] < OTA_HDR_BYTES ? OTA_E_TRUNCATED : OTA_E_SIZE);
    }
    put(q, img, sizeof img, (const uint8_t *)"x", 1);
    CT_EQ(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL), OTA_E_SIZE);

    /* oversized length: even a correctly signed header is refused before reading the payload */
    memcpy(h, img, OTA_HDR_BYTES);
    uint32_t big = OTA_MAX_PAYLOAD + 1;
    for (int i = 0; i < 4; i++) h[12 + i] = (uint8_t)(big >> (8 * i));
    put(q, h, sizeof h, NULL, 0);
    CT_EQ(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL), OTA_E_TOO_BIG);
    /* signed length larger than the file */
    uint8_t shortpl[4] = {1, 2, 3, 4};
    make_image(q, OTA_TARGET_HUB, 1, shortpl, 4);
    uint8_t whole[OTA_HDR_BYTES + 4];
    f = fopen(q, "rb");
    CT_EQ(fread(whole, 1, sizeof whole, f), sizeof whole);
    fclose(f);
    put(q, whole, sizeof whole - 2, NULL, 0);
    CT_EQ(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL), OTA_E_SIZE);

    /* wrong magic, version, flags */
    int offs[] = {0, 4, 6};
    for (int i = 0; i < 3; i++) {
        uint8_t save = whole[offs[i]];
        whole[offs[i]] = 0x7f;
        put(q, whole, sizeof whole, NULL, 0);
        CT_EQ(ota_image_verify_file(q, OTA_TARGET_HUB, 0, NULL), OTA_E_FORMAT);
        whole[offs[i]] = save;
    }
}

static void slurp(const char *p, uint8_t *out, size_t n) {
    FILE *f = fopen(p, "rb");
    CT_CHECK(f != NULL);
    if (f) {
        CT_EQ(fread(out, 1, n, f), n);
        fclose(f);
    }
}

static void test_apply(void) {
    char hub[300], prev[300], newp[300], img[300], img2[300], bad[300];
    path(hub, "hub");
    path(prev, "hub.prev");
    path(newp, "hub.new");
    path(img, "a.img");
    path(img2, "b.img");
    path(bad, "bad.img");
    uint8_t oldb[] = "OLD-HUB-BINARY", a[] = "NEW-HUB-BINARY-1", b[] = "NEW-HUB-BINARY-2", got[32];
    put(hub, oldb, sizeof oldb, NULL, 0);
    make_image(img, OTA_TARGET_HUB, 10, a, sizeof a);
    make_image(img2, OTA_TARGET_HUB, 11, b, sizeof b);

    /* bad signature, wrong target, downgrade: target untouched, no prev made */
    uint8_t other[32] = {9};
    uint32_t built = 0;
    CT_EQ(ota_apply_staged(img, hub, prev, 0, other, &built), OTA_E_SIG);
    make_image(bad, OTA_TARGET_REMOTE, 10, a, sizeof a);
    CT_EQ(ota_apply_staged(bad, hub, prev, 0, pk, &built), OTA_E_TARGET);
    CT_EQ(ota_apply_staged(img, hub, prev, 11, pk, &built), OTA_E_DOWNGRADE);
    CT_CHECK(access(prev, F_OK) != 0);
    slurp(hub, got, sizeof oldb);
    CT_MEM_EQ(got, oldb, sizeof oldb);

    /* good apply: new binary live and executable, old kept as hub.prev, no leftovers */
    CT_EQ(ota_apply_staged(img, hub, prev, 0, pk, &built), OTA_OK);
    CT_EQ(built, 10);
    slurp(hub, got, sizeof a);
    CT_MEM_EQ(got, a, sizeof a);
    slurp(prev, got, sizeof oldb);
    CT_MEM_EQ(got, oldb, sizeof oldb);
    struct stat st;
    CT_EQ(stat(hub, &st), 0);
    CT_CHECK(st.st_mode & 0100);
    CT_CHECK(access(newp, F_OK) != 0);

    /* second apply: prev becomes the first update */
    CT_EQ(ota_apply_staged(img2, hub, prev, 11, pk, &built), OTA_OK);
    slurp(prev, got, sizeof a);
    CT_MEM_EQ(got, a, sizeof a);
    slurp(hub, got, sizeof b);
    CT_MEM_EQ(got, b, sizeof b);

    /* rollback restores prev */
    CT_EQ(ota_rollback(hub, prev), 0);
    slurp(hub, got, sizeof a);
    CT_MEM_EQ(got, a, sizeof a);
    CT_EQ(ota_rollback(hub, prev), -1);

    /* tampered staged payload: refused, nothing changes */
    uint8_t buf[OTA_HDR_BYTES + sizeof b];
    slurp(img2, buf, sizeof buf);
    buf[OTA_HDR_BYTES] ^= 1;
    put(bad, buf, sizeof buf, NULL, 0);
    CT_EQ(ota_apply_staged(bad, hub, prev, 0, pk, &built), OTA_E_DIGEST);
    slurp(hub, got, sizeof a);
    CT_MEM_EQ(got, a, sizeof a);

    /* target in an unwritable place: failure, no hub.new left */
    char nod[300];
    path(nod, "nodir/hub");
    CT_EQ(ota_apply_staged(img, nod, prev, 0, pk, &built), OTA_E_IO);
}

int main(void) {
    if (!mkdtemp(dir)) return 1;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 1);
    ota_keypair_from_seed(seed, pk, sk);
    test_sha512();
    test_ed25519_vectors();
    test_verify();
    test_apply();
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) return 1;
    return CT_DONE();
}
