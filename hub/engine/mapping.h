#ifndef HP_ENGINE_MAPPING_H
#define HP_ENGINE_MAPPING_H

/* Button -> action tables and activity steps, pushed by Home Assistant via
 * `set_config` and executed through injectable sinks (hub/io/sinks.h).
 *
 * ---------------------------------------------------------------------
 * set_config JSON layout  (the `config` field of the set_config message)
 * ---------------------------------------------------------------------
 * {
 *   "keys": { "<key>": <action>, ... },          // optional: global key map
 *   "activities": [                               // optional: ordered list;
 *     {                                           // index == state_update.activity
 *       "name":  "Watch TV",                      // required, unique, 1..31 bytes
 *       "keys":  { "<key>": <action>, ... },      // optional: overrides global
 *       "steps": [ <action>, ... ],               // optional: run in order on start
 *       "off":   [ <action>, ... ]                // optional: run in order on power-off
 *     }
 *   ]
 * }
 *
 * <key>    decimal string of the key code, 0..65535, no sign/leading junk.
 *          Duplicate keys in one map are an error. The hub's key_event `key`
 *          (u8) is looked up as that code; `remote` is not part of the map.
 * <action> one of
 *   {"type":"ir", "port_mask":1..255, "pronto":[w0,w1,...]}
 *        pronto = 4..256 integers 0..65535 (the Pronto words, already
 *        numeric; the hub does not parse hex text).
 *   {"type":"bt", "usage":0..65535}     HID consumer usage; apply = key down then up
 *   {"type":"ha", "event":"<name>"}     forwarded to HA only (1..31 bytes); no sink call
 *   {"type":"none"}
 * Unknown "type", missing/ill-typed/out-of-range fields => load error.
 * Unknown extra object members are ignored.
 *
 * Example:
 *   {"keys":{"1":{"type":"ha","event":"menu"}},
 *    "activities":[{"name":"TV",
 *       "keys":{"2":{"type":"ir","port_mask":3,"pronto":[0,109,0,0]}},
 *       "steps":[{"type":"ir","port_mask":1,"pronto":[0,109,0,0]},{"type":"bt","usage":48}],
 *       "off":[{"type":"ir","port_mask":255,"pronto":[0,109,0,0]}]}]}
 *
 * Fixed limits (exceeding any is a load error, -HP_E_TOOBIG, never truncation):
 *   HP_MAP_MAX_JSON       65536 bytes of config text
 *   HP_MAP_MAX_ACTIVITIES 16
 *   HP_MAP_MAX_KEYS       64 entries per key map (global and per activity)
 *   HP_MAP_MAX_STEPS      16 steps in each of "steps" and "off"
 *   HP_MAP_MAX_PRONTO     256 words in one IR action
 *   HP_MAP_POOL_WORDS     8192 Pronto words across the whole config
 *   HP_MAP_NAME_MAX       32 (31 bytes + NUL) for activity and event names
 *
 * hp_map_load is all-or-nothing: parsed into a scratch map and swapped in on
 * success; on any error the previous map (and current activity) is untouched.
 * Key lookup/apply performs no dynamic allocation (loading and caching do).
 * ---------------------------------------------------------------------
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub/io/sinks.h"
#include "proto/gen/harmony_proto.h"

#define HP_MAP_MAX_JSON 65536
#define HP_MAP_MAX_ACTIVITIES 16
#define HP_MAP_MAX_KEYS 64
#define HP_MAP_MAX_STEPS 16
#define HP_MAP_MAX_PRONTO 256
#define HP_MAP_POOL_WORDS 8192
#define HP_MAP_NAME_MAX 32

typedef enum { HP_ACT_NONE = 0, HP_ACT_IR, HP_ACT_BT, HP_ACT_HA_EVENT } hp_act_type_t;

/* Tagged action. Pronto words live in the owning map's pool, so an
 * hp_action_t is only meaningful with the map it came from. */
typedef struct {
    hp_act_type_t type;
    uint8_t port_mask;               /* HP_ACT_IR */
    uint16_t pronto_off, pronto_n;   /* HP_ACT_IR: slice of the map pool */
    uint16_t usage;                  /* HP_ACT_BT */
    char event[HP_MAP_NAME_MAX];     /* HP_ACT_HA_EVENT */
} hp_action_t;

typedef struct {
    uint16_t code;
    hp_action_t act;
} hp_keybind_t;

typedef struct {
    char name[HP_MAP_NAME_MAX];
    uint8_t nkeys, nsteps, noff;
    hp_keybind_t keys[HP_MAP_MAX_KEYS];
    hp_action_t steps[HP_MAP_MAX_STEPS];
    hp_action_t off[HP_MAP_MAX_STEPS];
} hp_activity_t;

typedef struct {
    uint8_t nkeys, nacts;
    hp_keybind_t keys[HP_MAP_MAX_KEYS];
    hp_activity_t acts[HP_MAP_MAX_ACTIVITIES];
    uint16_t pool[HP_MAP_POOL_WORDS];
    size_t pool_n;
    int current;      /* running activity index, -1 = none */
    char *src;        /* verbatim config text (for the cache); owned */
    size_t src_len;
} hp_map_t;

void hp_map_init(hp_map_t *);
void hp_map_free(hp_map_t *);

/* 0 on success; -HP_E_INVAL (malformed/invalid), -HP_E_TOOBIG (limit). */
int hp_map_load(hp_map_t *, const char *json, size_t len);

/* Looks `key_code` up in activity `activity` first (NULL = skip), then the
 * global map. Returns 1 and fills *out if mapped; 0 and *out = {HP_ACT_NONE}
 * if not; -HP_E_INVAL for bad args or an unknown activity name. */
int hp_map_lookup(const hp_map_t *, const char *activity, uint16_t key_code, hp_action_t *out);

/* Runs one action through the sinks. IR: one ir->send. BT: key down, key up.
 * HA_EVENT / NONE: no sink call. Returns 0 or the sink's negative error;
 * -HP_E_INVAL if a needed sink is NULL. */
int hp_map_apply(const hp_map_t *, const hp_action_t *, const hp_sinks_t *);

size_t hp_map_activity_count(const hp_map_t *);
const char *hp_map_activity_name(const hp_map_t *, size_t index); /* NULL if out of range */
int hp_map_current(const hp_map_t *); /* -1 = none */

/* Runs the named activity's "steps" in order and makes it current. Returns
 * its index (>=0); -HP_E_INVAL for an unknown name; a step failure stops the
 * run, leaves the current activity unchanged and returns that error (steps
 * already sent are not undone). No implicit power-off of a previous activity. */
int hp_activity_set(hp_map_t *, const char *name, const hp_sinks_t *);
/* Runs the current activity's "off" steps and clears it. Returns the index of
 * the activity that was turned off; -HP_E_INVAL if none is current. */
int hp_activity_off(hp_map_t *, const hp_sinks_t *);

/* Incoming key from a remote: looks it up in the current activity (then
 * global) and, for PRESS only, applies it. `emit` (may be NULL) is always
 * called afterwards so HA sees every key, mapped or not. Returns the apply
 * result (0 when nothing to apply). */
typedef void (*hp_key_emit_fn)(void *ctx, uint8_t remote, uint8_t key, hp_key_action_t action);
int hp_key_handle(hp_map_t *, const hp_sinks_t *, uint8_t remote, uint8_t key,
                  hp_key_action_t action, hp_key_emit_fn emit, void *ctx);

/* Cache (HA-offline fallback). File = "HPMC" | version u8 (1) | len u32 BE |
 * config text | CRC-32 (IEEE) u32 BE over everything before it. Written to
 * "<path>.tmp.<pid>" in the same directory, fsync'd, then rename(2)d.
 * save: -HP_E_INVAL if no config is loaded, -HP_E_IO on I/O failure.
 * load: -HP_E_NOENT if missing, -HP_E_INVAL if corrupt/truncated/invalid
 * (map untouched), -HP_E_TOOBIG for an over-limit config, -HP_E_IO. */
int hp_map_save_cache(const hp_map_t *, const char *path);
int hp_map_load_cache(hp_map_t *, const char *path);

#endif
