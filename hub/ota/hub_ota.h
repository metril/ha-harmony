#ifndef HP_HUB_OTA_H
#define HP_HUB_OTA_H

/* Hub-side signed firmware update: receives a HOTA image in offset-addressed chunks into a staging
 * file, verifies and applies it with common/ota (ota_apply_staged), and asks the daemon to exit
 * with HP_EXIT_OTA_RESTART so the boot hook relaunches the new binary. Never replaces the running
 * process itself. Files (all in `dir`, the daemon's data directory; names are shared with
 * tools/boot/rcS.harmony):
 *   hub             the installed binary (target)        hub.prev          the build before the last update
 *   ota-staged.img  the upload in flight                 ota-build         build id of the last applied image
 *   ota-pending     "build=N" + "prev=M": a fresh build that has not reached the health mark yet. Written after
 *                   hub.prev is in place, immediately before the rename; removed by the hook at the health mark,
 *                   or by us if the apply fails. A cut before the rename leaves hub.prev == hub (same inode), so
 *                   the hook's rollback is a no-op.
 *   ota-build       the installed build (the hook restores M from "prev=M" on rollback)
 *   ota-restart     asks the hook to relaunch immediately (the exit status is not visible to it)
 *   ota-rollback    written by the hook after it restored hub.prev: "build=N" "reason=..." lines */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/ota/ota_image.h"

#define HP_EXIT_OTA_RESTART 42
#define HP_OTA_CHUNK_MAX 4096
#define HP_OTA_SLACK (64u * 1024u) /* free space kept beyond the 2x image */

enum {
    HP_OTA_OK = 0,
    HP_OTA_E_DISABLED = 1,
    HP_OTA_E_BUSY,
    HP_OTA_E_STATE,    /* no upload, or not yours */
    HP_OTA_E_OFFSET,   /* chunk offset ahead of what the hub holds; hub_ota.offset is the expected one */
    HP_OTA_E_SIZE,
    HP_OTA_E_TARGET,
    HP_OTA_E_CHUNK,
    HP_OTA_E_NOSPACE,
    HP_OTA_E_IO,
    HP_OTA_E_SHA,      /* transfer corrupted: whole-image digest differs from begin.sha */
    HP_OTA_E_VERIFY,   /* common/ota refused it (o->verify_rc: OTA_E_*) */
    HP_OTA_E_DOWNGRADE,
    HP_OTA_E_ARG,
    HP_OTA_E_NOTREADY, /* the hook has not marked this boot healthy, or an update is still unproven */
    HP_OTA_E_DEVKEY    /* dev key built in and --ota-allow-dev not given */
};

typedef struct {
    bool enabled;
    char dir[240];
    char staged[300], target[300], prev[300], pending[300], restart_file[300], build_file[300], rollback[300];
    const uint8_t *pubkey; /* default: the compiled-in key */
    uint32_t min_build;    /* the running build: max(ota-build file, compiled-in HP_BUILD_ID); a new image must be newer */
    bool key_dev, allow_dev; /* dev key built in / explicitly allowed (--ota-allow-dev) */
    const char *health_file; /* set by the hook at the health mark; NULL = not supervised, no check */
    bool receiving, owner_alive;
    const void *owner;
    uint32_t size, build, offset;
    uint8_t sha[64];
    ota_sha512_t ctx;
    int fd;
    int verify_rc;
    bool restart; /* applied: the daemon must exit HP_EXIT_OTA_RESTART once replies are flushed */
    /* test seams */
    uint64_t (*avail_fn)(const char *dir); /* free bytes, or UINT64_MAX when unknown */
    long (*write_fn)(int fd, const void *p, size_t n);
    void (*swap_hook)(void); /* runs right after the pending marker is written, before the rename */
} hp_ota_t;

/* dir NULL: updates disabled (every call returns HP_OTA_E_DISABLED). target NULL: dir/hub. Removes
 * leftovers of an interrupted run (staging file, <target>.new). */
void hp_ota_init(hp_ota_t *o, const char *dir, const char *target);
int hp_ota_begin(hp_ota_t *o, const void *owner, unsigned target, uint32_t size, uint32_t build, const char *sha_hex);
int hp_ota_chunk(hp_ota_t *o, const void *owner, uint32_t offset, const uint8_t *data, size_t n);
int hp_ota_end(hp_ota_t *o, const void *owner, uint32_t *build_out);
int hp_ota_abort(hp_ota_t *o, const void *owner);
/* The owner's connection closed: the upload stays resumable by any matching begin. */
void hp_ota_owner_gone(hp_ota_t *o, const void *owner);
/* Stable wire code ("ota_busy", ...) and a human message. */
const char *hp_ota_code(int e);
const char *hp_ota_message(const hp_ota_t *o, int e);
/* The hook's rollback record: returns 1 and fills build/reason when present. */
int hp_ota_rollback_info(const hp_ota_t *o, uint32_t *build, char *reason, size_t cap);

#endif
