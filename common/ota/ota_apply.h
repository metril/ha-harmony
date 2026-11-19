#ifndef OTA_APPLY_H
#define OTA_APPLY_H

#include "ota_image.h"

/* Hub-side staged apply: verify the staged image (target hub, build >= min_build), write its
 * payload next to target as <target>.new, keep the current binary as prev_path (hard link, so
 * target never disappears), then atomically rename .new over target. Any failure leaves target
 * untouched. Not wired into the boot hook: the caller decides when to run it. Returns OTA_OK
 * or an OTA_E_* code; *build_out receives the applied build id. */
int ota_apply_staged(const char *staged, const char *target, const char *prev_path, uint32_t min_build,
                     const uint8_t pubkey[32], uint32_t *build_out);

/* Same, calling before_swap(ctx) (0 = go on; non-zero aborts the rename: OTA_E_IO, target untouched) after hub.prev is in place and immediately before the final rename (the
 * hub arms its rollback marker there: a cut earlier leaves hub.prev identical to the binary). */
int ota_apply_staged_cb(const char *staged, const char *target, const char *prev_path, uint32_t min_build,
                        const uint8_t pubkey[32], uint32_t *build_out, int (*before_swap)(void *), void *ctx);

/* Restore prev_path over target (atomic rename). Returns 0 or -1. */
int ota_rollback(const char *target, const char *prev_path);

extern void (*ota_apply_test_hook)(void); /* tests only */

#endif
