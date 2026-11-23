#define _GNU_SOURCE
#include "hub/ota/hub_ota.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "common/ota/ota_apply.h"

#ifndef HP_BUILD_ID
#define HP_BUILD_ID 0 /* set by CMake (git commit count, floored by cmake/build_id_floor.txt) */
#endif
#define HP_STR2(x) #x
#define HP_STR(x) HP_STR2(x)
/* tools/ota_sign reads the build id from here when -b is omitted, so image and binary cannot disagree */
const char hp_build_tag[] = "HPBUILDID=" HP_STR(HP_BUILD_ID) ";";
uint32_t (*hp_build_id_hook)(void); /* tests only: replaces the compiled-in id */
uint32_t hp_build_id(void) {
    if (hp_build_id_hook) return hp_build_id_hook();
    /* the volatile read references hp_build_tag, so --gc-sections keeps the string ota_sign looks for */
    return *(const volatile char *)hp_build_tag == 'H' ? (uint32_t)HP_BUILD_ID : 0;
}

static uint64_t default_avail(const char *dir) {
    struct statvfs v;
    if (statvfs(dir, &v) != 0 || v.f_blocks == 0) return UINT64_MAX; /* unknown: rely on ENOSPC */
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

static long default_write(int fd, const void *p, size_t n) { return (long)write(fd, p, n); }

static int join(char *out, size_t cap, const char *dir, const char *name) {
    return snprintf(out, cap, "%s/%s", dir, name) < (int)cap ? 0 : -1;
}

static int write_small(const char *path, const char *text) { /* tmp + fsync + rename; 0 on success */
    char tmp[310];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && fsync(fd) == 0;
    close(fd);
    if (ok && rename(tmp, path) == 0) return 0;
    unlink(tmp);
    return -1;
}

void hp_ota_init(hp_ota_t *o, const char *dir, const char *target) {
    memset(o, 0, sizeof *o);
    o->fd = -1;
    o->done[0] = o->done[1] = -1;
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
    o->min_build = hp_build_id();
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
    if (o->ending) return HP_OTA_E_BUSY;
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
    if (o->ending) return HP_OTA_E_BUSY;
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

/* Between "hub.prev is the old binary" and the rename: the only moment the marker may exist. Both files must
 * be durable or the rename does not happen (no swap without a way back). */
static int arm_pending(void *v) {
    arm_t *a = v;
    char note[64];
    snprintf(note, sizeof note, "build=%" PRIu32 "\nprev=%" PRIu32 "\n", a->build, a->o->min_build);
    if (write_small(a->o->pending, note) != 0) return -1;
    snprintf(note, sizeof note, "%" PRIu32 "\n", a->build);
    if (write_small(a->o->build_file, note) != 0) return -1; /* the hook puts "prev=" back if this build fails */
    if (a->o->swap_hook) a->o->swap_hook();
    return 0;
}

/* The slow half of the end: runs on the worker thread. It only touches files and o->job; the loop-visible state
 * (receiving, owner, min_build, restart) is updated by hp_ota_end_done, back on the loop. */
static void *end_worker(void *arg) {
    hp_ota_t *o = arg;
    int rc = HP_OTA_OK;
    uint32_t built = 0;
    int verify_rc = o->verify_rc;
    uint8_t dig[64];
    ota_sha512_final(&o->ctx, dig);
    int fsync_ok = fsync(o->fd) == 0;
    int close_ok = close(o->fd) == 0;
    o->fd = -1;
    uint32_t declared = o->build;
    if (memcmp(dig, o->sha, 64) != 0) {
        rc = HP_OTA_E_SHA;
    } else if (!fsync_ok || !close_ok) {
        rc = HP_OTA_E_IO;
    } else {
        uint8_t hb[12];
        int hfd = open(o->staged, O_RDONLY);
        int hok = hfd >= 0 && read(hfd, hb, sizeof hb) == (ssize_t)sizeof hb;
        if (hfd >= 0) close(hfd);
        uint32_t signed_build = hok ? (uint32_t)hb[8] | (uint32_t)hb[9] << 8 | (uint32_t)hb[10] << 16 | (uint32_t)hb[11] << 24 : 0;
        if (!hok || signed_build != declared) { /* refuse before anything is swapped */
            verify_rc = hok ? OTA_E_FORMAT : OTA_E_IO;
            rc = hok ? HP_OTA_E_VERIFY : HP_OTA_E_IO;
        } else {
            char note[48];
            arm_t arm = {o, declared};
            int ar = ota_apply_staged_cb(o->staged, o->target, o->prev, o->min_build + 1, o->pubkey, &built, arm_pending, &arm);
            if (ar != OTA_OK) {
                verify_rc = ar;
                if (access(o->pending, F_OK) == 0) { /* armed, then the rename failed: undo */
                    unlink(o->pending);
                    snprintf(note, sizeof note, "%" PRIu32 "\n", o->min_build);
                    write_small(o->build_file, note);
                }
                rc = ar == OTA_E_DOWNGRADE ? HP_OTA_E_DOWNGRADE : ar == OTA_E_IO ? HP_OTA_E_IO : HP_OTA_E_VERIFY;
            } else {
                snprintf(note, sizeof note, "%" PRIu32 "\n", built);
                unlink(o->rollback);
                write_small(o->restart_file, note);
            }
        }
    }
    unlink(o->staged); /* applied or not, the staging file is spent (a failed upload is never resumable) */
    o->job.rc = rc;
    o->job.built = built;
    o->job.verify_rc = verify_rc;
    char b = 1;
    (void)!write(o->done[1], &b, 1); /* last: the loop reads job after this */
    return NULL;
}

int hp_ota_end_start(hp_ota_t *o, const void *owner) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (o->ending) return HP_OTA_E_BUSY;
    if (!mine(o, owner)) return HP_OTA_E_STATE;
    if (o->offset != o->size) return HP_OTA_E_SIZE; /* still resumable */
    if (o->done[0] < 0 && pipe2(o->done, O_NONBLOCK | O_CLOEXEC) < 0) {
        o->done[0] = o->done[1] = -1;
        return HP_OTA_E_IO; /* still resumable */
    }
    o->ending = true;
    if (pthread_create(&o->worker, NULL, end_worker, o) != 0) {
        o->ending = false;
        return HP_OTA_E_IO; /* still resumable */
    }
    return HP_OTA_ASYNC;
}

int hp_ota_done_fd(const hp_ota_t *o) { return o->done[0]; }
bool hp_ota_ending(const hp_ota_t *o) { return o->ending; }

int hp_ota_end_done(hp_ota_t *o, uint32_t *build_out, const void **owner_out, bool *owner_alive_out) {
    if (!o->ending) return HP_OTA_E_STATE;
    char junk[8];
    while (read(o->done[0], junk, sizeof junk) > 0) {}
    pthread_join(o->worker, NULL); /* the worker has written the completion byte as its last act: this returns at once */
    o->ending = false;
    if (owner_out) *owner_out = o->owner;
    if (owner_alive_out) *owner_alive_out = o->owner_alive;
    int rc = o->job.rc;
    o->verify_rc = o->job.verify_rc;
    o->fd = -1;
    o->receiving = false;
    o->owner = NULL;
    o->owner_alive = false;
    o->offset = 0;
    if (rc == HP_OTA_OK) {
        o->min_build = o->job.built;
        o->restart = true;
        if (build_out) *build_out = o->job.built;
    }
    return rc;
}

int hp_ota_end(hp_ota_t *o, const void *owner, uint32_t *build_out) {
    int rc = hp_ota_end_start(o, owner);
    if (rc != HP_OTA_ASYNC) return rc;
    struct pollfd p = {o->done[0], POLLIN, 0};
    while (poll(&p, 1, -1) < 0 && errno == EINTR) {}
    return hp_ota_end_done(o, build_out, NULL, NULL);
}

int hp_ota_abort(hp_ota_t *o, const void *owner) {
    if (!o->enabled) return HP_OTA_E_DISABLED;
    if (o->ending) return HP_OTA_E_BUSY;
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
