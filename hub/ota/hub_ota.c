#define _DEFAULT_SOURCE
#include "hub/ota/hub_ota.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "common/ota/ota_apply.h"

#ifndef HP_BUILD_ID
#define HP_BUILD_ID 0 /* -DHP_BUILD_ID=N at configure time: the build id of this binary */
#endif

static uint64_t default_avail(const char *dir) {
    struct statvfs v;
    if (statvfs(dir, &v) != 0 || v.f_blocks == 0) return UINT64_MAX; /* unknown: rely on ENOSPC */
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

static long default_write(int fd, const void *p, size_t n) { return (long)write(fd, p, n); }

static int join(char *out, size_t cap, const char *dir, const char *name) {
    return snprintf(out, cap, "%s/%s", dir, name) < (int)cap ? 0 : -1;
}

static void write_small(const char *path, const char *text) { /* tmp + rename, best effort */
    char tmp[310];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && fsync(fd) == 0;
    close(fd);
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

void hp_ota_init(hp_ota_t *o, const char *dir, const char *target) {
    memset(o, 0, sizeof *o);
    o->fd = -1;
    o->pubkey = ota_pubkey();
    o->key_dev = ota_pubkey_is_dev() != 0;
    o->avail_fn = default_avail;
    o->write_fn = default_write;
    if (!dir || strlen(dir) >= sizeof o->dir - 1) return;
    strcpy(o->dir, dir);
    int bad = 0;
    bad |= join(o->staged, sizeof o->staged, dir, "ota-staged.img");
    bad |= join(o->prev, sizeof o->prev, dir, "hub.prev");
    bad |= join(o->pending, sizeof o->pending, dir, "ota-pending");
    bad |= join(o->restart_file, sizeof o->restart_file, dir, "ota-restart");
    bad |= join(o->build_file, sizeof o->build_file, dir, "ota-build");
    bad |= join(o->rollback, sizeof o->rollback, dir, "ota-rollback");
    if (target) bad |= snprintf(o->target, sizeof o->target, "%s", target) >= (int)sizeof o->target;
    else bad |= join(o->target, sizeof o->target, dir, "hub");
    if (bad) return;
    o->enabled = true;
    /* interrupted run: the partial upload and a half-written <target>.new are never useful */
    unlink(o->staged);
    char leftover[320];
    if (snprintf(leftover, sizeof leftover, "%s.new", o->target) < (int)sizeof leftover) unlink(leftover);
    if (snprintf(leftover, sizeof leftover, "%s.lnk", o->prev) < (int)sizeof leftover) unlink(leftover);
    unlink(o->restart_file); /* a restart request belongs to the process that wrote it, never the next one */
    o->min_build = (uint32_t)HP_BUILD_ID;
    FILE *f = fopen(o->build_file, "r");
    if (f) {
        unsigned long v;
        if (fscanf(f, "%lu", &v) == 1 && v <= UINT32_MAX && v > o->min_build) o->min_build = (uint32_t)v;
        fclose(f);
    }
}

static void drop_upload(hp_ota_t *o) {
    if (o->fd >= 0) close(o->fd);
    o->fd = -1;
    o->receiving = false;
    o->owner = NULL;
    o->owner_alive = false;
    o->offset = 0;
    unlink(o->staged);
}

static int hex_digest(const char *s, uint8_t out[64]) {
    if (!s || strlen(s) != 128) return -1;
    for (int i = 0; i < 64; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                                : 99u;
            if (d > 15) return -1;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

int hp_ota_begin(hp_ota_t *o, const void *owner, unsigned target, uint32_t size, uint32_t build, const char *sha_hex) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (o->key_dev && !o->allow_dev) return HP_OTA_E_DEVKEY;
    if (o->health_file && access(o->health_file, F_OK) != 0) return HP_OTA_E_NOTREADY;
    if (access(o->pending, F_OK) == 0) return HP_OTA_E_NOTREADY;
    uint8_t sha[64];
    if (hex_digest(sha_hex, sha) != 0) return HP_OTA_E_ARG;
    if (target != OTA_TARGET_HUB) return HP_OTA_E_TARGET;
    if (size < OTA_HDR_BYTES || size > OTA_HDR_BYTES + OTA_MAX_PAYLOAD) return HP_OTA_E_SIZE;
    if (build <= o->min_build) return HP_OTA_E_DOWNGRADE; /* older than or the same as what runs */
    if (o->receiving) {
        bool same = o->size == size && o->build == build && !memcmp(o->sha, sha, 64);
        if (o->owner_alive && (o->owner != owner || !same)) return HP_OTA_E_BUSY;
        if (same) { /* resume: continue at o->offset */
            o->owner = owner;
            o->owner_alive = true;
            return HP_OTA_OK;
        }
        drop_upload(o); /* a suspended upload of something else */
    }
    uint64_t avail = o->avail_fn(o->dir);
    if (avail != UINT64_MAX && avail < 2ull * size + HP_OTA_SLACK) return HP_OTA_E_NOSPACE;
    o->fd = open(o->staged, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (o->fd < 0) return errno == ENOSPC ? HP_OTA_E_NOSPACE : HP_OTA_E_IO;
    o->receiving = true;
    o->owner = owner;
    o->owner_alive = true;
    o->size = size;
    o->build = build;
    o->offset = 0;
    memcpy(o->sha, sha, 64);
    ota_sha512_init(&o->ctx);
    return HP_OTA_OK;
}

static bool mine(const hp_ota_t *o, const void *owner) { return o->receiving && o->owner_alive && o->owner == owner; }

int hp_ota_chunk(hp_ota_t *o, const void *owner, uint32_t offset, const uint8_t *data, size_t n) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (!mine(o, owner)) return HP_OTA_E_STATE;
    if (n == 0 || n > HP_OTA_CHUNK_MAX) return HP_OTA_E_CHUNK;
    if (offset > o->offset) return HP_OTA_E_OFFSET;
    if ((uint64_t)offset + n <= o->offset) return HP_OTA_OK; /* a retransmit of what we already hold */
    size_t skip = o->offset - offset;
    data += skip;
    n -= skip;
    if ((uint64_t)o->offset + n > o->size) return HP_OTA_E_SIZE;
    for (size_t done = 0; done < n;) {
        long w = o->write_fn(o->fd, data + done, n - done);
        if (w <= 0) {
            int e = w < 0 ? errno : EIO;
            drop_upload(o); /* never leave a partial file behind */
            return e == ENOSPC || e == EDQUOT ? HP_OTA_E_NOSPACE : HP_OTA_E_IO;
        }
        done += (size_t)w;
    }
    ota_sha512_update(&o->ctx, data, n);
    o->offset += (uint32_t)n;
    return HP_OTA_OK;
}

typedef struct {
    hp_ota_t *o;
    uint32_t build;
} arm_t;

/* Between "hub.prev is the old binary" and the rename: the only moment the marker may exist. */
static void arm_pending(void *v) {
    arm_t *a = v;
    char note[64];
    snprintf(note, sizeof note, "build=%" PRIu32 "\nprev=%" PRIu32 "\n", a->build, a->o->min_build);
    write_small(a->o->pending, note);
    snprintf(note, sizeof note, "%" PRIu32 "\n", a->build);
    write_small(a->o->build_file, note); /* the hook puts "prev=" back if this build does not prove itself */
    if (a->o->swap_hook) a->o->swap_hook();
}

int hp_ota_end(hp_ota_t *o, const void *owner, uint32_t *build_out) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (!mine(o, owner)) return HP_OTA_E_STATE;
    if (o->offset != o->size) return HP_OTA_E_SIZE; /* still resumable */
    uint8_t dig[64];
    ota_sha512_final(&o->ctx, dig);
    int fsync_ok = fsync(o->fd) == 0;
    int close_ok = close(o->fd) == 0;
    o->fd = -1;
    if (memcmp(dig, o->sha, 64) != 0) {
        drop_upload(o);
        return HP_OTA_E_SHA;
    }
    if (!fsync_ok || !close_ok) {
        drop_upload(o);
        return HP_OTA_E_IO;
    }
    uint32_t declared = o->build;
    uint8_t hb[12];
    int hfd = open(o->staged, O_RDONLY);
    int hok = hfd >= 0 && read(hfd, hb, sizeof hb) == (ssize_t)sizeof hb;
    if (hfd >= 0) close(hfd);
    uint32_t signed_build = hok ? (uint32_t)hb[8] | (uint32_t)hb[9] << 8 | (uint32_t)hb[10] << 16 | (uint32_t)hb[11] << 24 : 0;
    if (!hok || signed_build != declared) { /* refuse before anything is swapped */
        o->verify_rc = hok ? OTA_E_FORMAT : OTA_E_IO;
        drop_upload(o);
        return hok ? HP_OTA_E_VERIFY : HP_OTA_E_IO;
    }
    char note[48];
    arm_t arm = {o, declared};
    uint32_t built = 0;
    int rc = ota_apply_staged_cb(o->staged, o->target, o->prev, o->min_build + 1, o->pubkey, &built, arm_pending, &arm);
    if (rc != OTA_OK) {
        o->verify_rc = rc;
        if (access(o->pending, F_OK) == 0) { /* armed, then the rename failed: undo */
            unlink(o->pending);
            snprintf(note, sizeof note, "%" PRIu32 "\n", o->min_build);
            write_small(o->build_file, note);
        }
        drop_upload(o);
        return rc == OTA_E_DOWNGRADE ? HP_OTA_E_DOWNGRADE : rc == OTA_E_IO ? HP_OTA_E_IO : HP_OTA_E_VERIFY;
    }
    snprintf(note, sizeof note, "%" PRIu32 "\n", built);
    o->min_build = built;
    unlink(o->rollback);
    unlink(o->staged);
    o->receiving = false;
    o->owner = NULL;
    o->owner_alive = false;
    o->offset = 0;
    o->restart = true;
    write_small(o->restart_file, note);
    if (build_out) *build_out = built;
    return HP_OTA_OK;
}

int hp_ota_abort(hp_ota_t *o, const void *owner) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (!o->receiving) return HP_OTA_OK;
    if (o->owner_alive && o->owner != owner) return HP_OTA_E_BUSY;
    drop_upload(o);
    return HP_OTA_OK;
}

void hp_ota_owner_gone(hp_ota_t *o, const void *owner) {
    if (o->receiving && o->owner == owner) o->owner_alive = false;
}

const char *hp_ota_code(int e) {
    switch (e) {
    case HP_OTA_E_DISABLED: return "ota_unsupported";
    case HP_OTA_E_BUSY: return "ota_busy";
    case HP_OTA_E_STATE: return "ota_state";
    case HP_OTA_E_OFFSET: return "ota_offset";
    case HP_OTA_E_SIZE: return "ota_size";
    case HP_OTA_E_TARGET: return "ota_target";
    case HP_OTA_E_CHUNK: return "ota_chunk";
    case HP_OTA_E_NOSPACE: return "ota_no_space";
    case HP_OTA_E_IO: return "ota_io";
    case HP_OTA_E_SHA: return "ota_sha";
    case HP_OTA_E_VERIFY: return "ota_verify";
    case HP_OTA_E_DOWNGRADE: return "ota_downgrade";
    case HP_OTA_E_NOTREADY: return "ota_not_ready";
    case HP_OTA_E_DEVKEY: return "ota_dev_key";
    default: return "ota_bad_request";
    }
}

const char *hp_ota_message(const hp_ota_t *o, int e) {
    switch (e) {
    case HP_OTA_E_DISABLED: return "this hub was started without firmware updates";
    case HP_OTA_E_BUSY: return "another update upload is in progress";
    case HP_OTA_E_STATE: return "no update upload in progress on this connection";
    case HP_OTA_E_OFFSET: return "chunk offset is ahead of the received data";
    case HP_OTA_E_SIZE: return "image size or received length is wrong";
    case HP_OTA_E_TARGET: return "image is not for the hub";
    case HP_OTA_E_CHUNK: return "chunk size must be 1..4096 bytes";
    case HP_OTA_E_NOSPACE: return "not enough free space on the hub";
    case HP_OTA_E_IO: return "hub storage error";
    case HP_OTA_E_SHA: return "image was corrupted in transfer (digest mismatch)";
    case HP_OTA_E_VERIFY: return ota_strerror(o->verify_rc);
    case HP_OTA_E_DOWNGRADE: return "image build is not newer than the installed one";
    case HP_OTA_E_NOTREADY: return "the hub has not confirmed its current build healthy yet; try again in a few minutes";
    case HP_OTA_E_DEVKEY: return "this hub trusts the public dev key; start it with --ota-allow-dev to accept updates";
    default: return "bad update request";
    }
}

int hp_ota_rollback_info(const hp_ota_t *o, uint32_t *build, char *reason, size_t cap) {
    if (!o->enabled) return 0;
    FILE *f = fopen(o->rollback, "r");
    if (!f) return 0;
    char line[160];
    *build = 0;
    snprintf(reason, cap, "rolled back");
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "build=", 6)) *build = (uint32_t)strtoul(line + 6, NULL, 10);
        else if (!strncmp(line, "reason=", 7)) snprintf(reason, cap, "%s", line + 7);
    }
    fclose(f);
    return 1;
}
