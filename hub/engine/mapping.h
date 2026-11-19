#ifndef HP_ENGINE_MAPPING_H
#define HP_ENGINE_MAPPING_H

/* Button -> action tables, devices and activity steps, pushed by Home Assistant via `set_config`
 * and executed through injectable sinks (hub/io/sinks.h).
 *
 * Two config formats are accepted, selected by the optional top-level "format" (absent = 1).
 * Format 2 is specified in docs/proto/config-format-2.md; this header records what the engine
 * implements of it and the format-1 layout.
 *
 * ---------------------------------------------------------------------
 * Format 1
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
 * <key>    decimal string of the key code, 0..65535. Duplicate keys in one map are an error.
 * <action> one of
 *   {"type":"ir", "port_mask":1..255, "pronto":[w0,w1,...]}   Pronto words as integers (4..512)
 *   {"type":"bt", "usage":0..65535}     HID consumer usage; apply = key down then up
 *   {"type":"ha", "event":"<name>"}     forwarded to HA only (1..31 bytes); no sink call
 *   {"type":"none"}
 *
 * ---------------------------------------------------------------------
 * Format 2 (differences)
 * ---------------------------------------------------------------------
 * "format":2, "devices":[{id,name?,power?,inputs?,settle_ms?,commands?}], activities with "id",
 * optional "name" (defaults to id), "devices":[{device,input?}] (references validated, stored for
 * the activity state machine), key codes 0..255.
 * IR action = exactly one of {proto,addr,cmd} | {raw:{hz,us}} | {pronto:"hex words"} plus optional
 * "port_mask" (default 255). A "type":"ir" action carries those fields inline.
 * Further action types: {"type":"device","device":id,"command":name | "input":name}
 * (command may also be "power_on"/"power_off"), resolved at load time; and
 * {"type":"activity","activity":"<id>"|"off"} (key maps only, not in steps/off).
 * Every IR action is run through the encoder at load time; a bad code is a load error.
 *
 * Memory budget instead of count limits. Devices' commands and inputs, the IR word pool and the key
 * maps are allocated per load, exactly as large as the config needs. The only size limit is ONE
 * byte budget (hp_map_budget, default HP_MAP_BUDGET_BYTES) on what loading a config occupies at
 * its peak: HP_MAP_EST_TEXT_COPIES copies of the text + the parse tree nodes + the tables (see
 * hp_map_estimate). A config over it is rejected before anything is allocated, with "config
 * needs ~N KiB of hub memory, the hub allows M KiB". Commands are found through a hash index
 * built at load time (hp_map_find_cmd / hp_map_find_input), never by a linear scan.
 *
 * Per-item bounds that remain (a load error, -HP_E_TOOBIG, never truncation):
 *   HP_MAP_MAX_DEVICES    255  devices: uint8 index in the activity plan, state file and device_state fan-out
 *   HP_MAP_MAX_ACTIVITIES 16   activities: uint8 index on the wire (state_update.activity)
 *   HP_MAP_MAX_KEYS       256  entries per key map: the key code space (codes 0..255) of a remote
 *   HP_MAP_MAX_STEPS      16   steps in each of "steps" and "off" (activity plan size)
 *   HP_MAP_MAX_ACT_DEVS   8    devices referenced by one activity (activity plan size)
 *   HP_MAP_MAX_PRONTO     520  words in one Pronto IR action; raw: 512 durations (encoder)
 *   HP_MAP_NAME_MAX       32   (31 bytes + NUL) for ids, names, commands, events
 *
 * hp_map_load is all-or-nothing: parsed into a scratch map and swapped in on success; on any
 * error (including an allocation failure, -HP_E_IO) the previous map (and current activity) is
 * untouched and hp_map_last_error() describes the first problem. Key lookup/apply performs no
 * dynamic allocation (loading and caching do).
 * ---------------------------------------------------------------------
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub/engine/irout.h"
#include "hub/io/sinks.h"
#include "proto/gen/harmony_proto.h"
#include "vendor/cjson/cJSON.h"

#define HP_MAP_BUDGET_BYTES (8u * 1024u * 1024u) /* hub default: ~20% of its free RAM, see docs/proto/config-format-2.md */
#define HP_MAP_MAX_DEVICES 255
#define HP_MAP_MAX_ACTIVITIES 16
#define HP_MAP_MAX_KEYS 256
#define HP_MAP_MAX_STEPS 16
#define HP_MAP_MAX_ACT_DEVS 8
#define HP_MAP_MAX_PRONTO 520
/* Estimate model (hp_map_estimate); the hub reports these in hello_ack.limits so clients compute
 * the same number. *_BYTES include the hash index slot(s) of the entry. */
#define HP_MAP_EST_TEXT_COPIES 3 /* the received text, the map's cache copy, the parse tree's strings */
#define HP_MAP_NODE_BYTES 80     /* one parsed JSON value: cJSON node + malloc overhead + its strings' overhead */
#define HP_MAP_DEV_BYTES 224
#define HP_MAP_CMD_BYTES 120     /* one command or input */
#define HP_MAP_KEY_BYTES 72
#define HP_MAP_WORD_BYTES 2
#define HP_MAP_NAME_MAX 32
#define HP_MAP_ERR_MAX 96

#define HP_TARGET_OFF 0xFF /* HP_ACT_ACTIVITY target meaning "power down" */

typedef enum { HP_ACT_NONE = 0, HP_ACT_IR, HP_ACT_BT, HP_ACT_HA_EVENT, HP_ACT_ACTIVITY } hp_act_type_t;

/* Tagged action. IR data lives in the owning map's pool, so an hp_action_t is only meaningful
 * with the map it came from. */
typedef struct {
    hp_act_type_t type;
    uint8_t port_mask;             /* HP_ACT_IR */
    uint8_t repeats;               /* HP_ACT_IR: frames per send, 1..8 (0 = 1); format 2 `repeats` */
    uint8_t ir_kind;               /* HP_ACT_IR: hp_ir_kind_t */
    uint8_t proto;                 /* HP_IR_CODE_PROTO: hp_ir_proto_t */
    uint8_t target;                /* HP_ACT_ACTIVITY: activity index or HP_TARGET_OFF */
    uint32_t pool_off;             /* PRONTO/RAW: first word of the slice in the map pool */
    uint16_t pool_n;               /* PRONTO: words; RAW: durations */
    uint32_t addr, cmd;            /* PROTO */
    uint32_t hz;                   /* RAW */
    uint16_t usage;                /* HP_ACT_BT */
    char event[HP_MAP_NAME_MAX];   /* HP_ACT_HA_EVENT */
} hp_action_t;

typedef struct {
    uint16_t code;
    hp_action_t act;
} hp_keybind_t;

typedef enum { HP_PWR_NONE = 0, HP_PWR_ONOFF, HP_PWR_TOGGLE } hp_pwr_kind_t;

typedef struct {
    char id[HP_MAP_NAME_MAX];
    char name[HP_MAP_NAME_MAX];
    hp_pwr_kind_t pwr;
    hp_action_t pwr_on;   /* ONOFF: on; TOGGLE: the toggle code */
    hp_action_t pwr_off;  /* ONOFF only */
    uint16_t settle_ms;
} hp_device_t;

typedef struct {
    uint8_t dev;
    char name[HP_MAP_NAME_MAX];
    hp_action_t act;
} hp_devcmd_t; /* a command or an input of devs[dev] */

typedef struct {
    uint8_t dev;
    int32_t input; /* index into map.inputs, -1 = none */
} hp_actdev_t;

typedef struct {
    char id[HP_MAP_NAME_MAX];
    char name[HP_MAP_NAME_MAX];
    uint16_t nkeys;
    uint8_t nsteps, noff, ndevs;
    hp_actdev_t devs[HP_MAP_MAX_ACT_DEVS];
    hp_keybind_t *keys; /* nkeys entries, owned by the map */
    hp_action_t steps[HP_MAP_MAX_STEPS];
    hp_action_t off[HP_MAP_MAX_STEPS];
} hp_activity_t;

/* Activity hooks installed by the activity state machine (hub/engine/activity.h); when set,
 * "activity" key actions go through it instead of hp_activity_set/off, and while busy() holds,
 * non-activity key actions are dropped (their frames would overlap the transition). */
typedef struct {
    void *ctx;
    int (*start)(void *ctx, int idx);
    int (*off)(void *ctx);
    bool (*busy)(void *ctx);
} hp_map_actops_t;

typedef struct {
    int format;
    uint8_t ndevs, nacts;
    uint16_t nkeys;
    uint32_t ncmds, ninputs;
    hp_keybind_t *keys;    /* global key map: nkeys entries */
    hp_device_t *devs;     /* ndevs entries */
    hp_devcmd_t *cmds;     /* ncmds entries */
    hp_devcmd_t *inputs;   /* ninputs entries */
    hp_activity_t acts[HP_MAP_MAX_ACTIVITIES];
    uint16_t *pool;        /* Pronto words / raw durations of the whole config */
    size_t pool_n, pool_cap;
    uint32_t *idx_dev, *idx_cmd, *idx_in; /* open-addressing hash indexes (entry index + 1, 0 = empty) */
    uint32_t mask_dev, mask_cmd, mask_in; /* slot count - 1 */
    size_t budget;         /* byte budget of one load; 0 = HP_MAP_BUDGET_BYTES. Survives reloads */
    size_t est_bytes;      /* hp_map_estimate of the loaded config (0 = none) */
    int current;      /* running activity index, -1 = none */
    char *src;        /* verbatim config text (for the cache); owned */
    size_t src_len;
    uint32_t hash;    /* CRC-32 of src */
    char err[HP_MAP_ERR_MAX]; /* first problem of the last failed load */
    hp_irout_t *out;  /* IR output runtime (hold/repeat/toggle); optional, survives reloads */
    hp_map_actops_t actops; /* optional, survives reloads */
} hp_map_t;

void hp_map_init(hp_map_t *);
/* Frees everything the map owns (tables, pool, indexes, text); the map is then empty but usable. */
void hp_map_free(hp_map_t *);
/* Sets the byte budget of one load (0 = HP_MAP_BUDGET_BYTES). */
void hp_map_set_budget(hp_map_t *, size_t bytes);
size_t hp_map_budget(const hp_map_t *);
/* Peak-load estimate of the config text in bytes (0 on invalid JSON); fills the byte total in *est.
 * Same formula the clients use (hello_ack.limits). -HP_E_INVAL / -HP_E_IO on failure. */
int hp_map_estimate(const char *json, size_t len, size_t *est);
/* Index of the command / input `name` of device `dev`, or -1. O(1). */
int hp_map_find_cmd(const hp_map_t *, int dev, const char *name);
int hp_map_find_input(const hp_map_t *, int dev, const char *name);
/* Test hook: the n-th allocation of the next load fails (1 = the first); 0 = off. */
void hp_map_test_fail_alloc(unsigned n);
/* Attaches the IR output runtime (NULL = send every IR action once, no timers). */
void hp_map_set_out(hp_map_t *, hp_irout_t *);
void hp_map_set_actops(hp_map_t *, const hp_map_actops_t *);
/* CRC-32 (IEEE) as used for the config hash, the cache and the state file. */
uint32_t hp_map_crc32(const uint8_t *p, size_t n);

/* 0 on success; -HP_E_INVAL (malformed/invalid), -HP_E_TOOBIG (limit). */
int hp_map_load(hp_map_t *, const char *json, size_t len);
/* Human-readable reason for the last failed hp_map_load ("" if none). */
const char *hp_map_last_error(const hp_map_t *);
/* Lowercase hex CRC-32 of the loaded config text into out[9]; false (out = "") if none loaded. */
bool hp_map_hash_hex(const hp_map_t *, char out[9]);

/* Parses one ad-hoc format-2 IR action object ({proto,addr,cmd} | {raw} | {pronto}, optional
 * port_mask) into *a, storing its data in `scratch`'s pool (a zeroed hp_map_t; reuse of one
 * scratch across calls is not supported) and validating it with the encoder. 0 or a negative
 * error with the reason in hp_map_last_error(scratch). Apply it with hp_map_apply(scratch, ..). */
int hp_map_parse_ir(hp_map_t *scratch, const cJSON *j, hp_action_t *a);

/* Looks `key_code` up in activity `activity` first (NULL = skip), then the
 * global map. Returns 1 and fills *out if mapped; 0 and *out = {HP_ACT_NONE}
 * if not; -HP_E_INVAL for bad args or an unknown activity name. */
int hp_map_lookup(const hp_map_t *, const char *activity, uint16_t key_code, hp_action_t *out);

/* Builds the encoder input for an IR action. `raw` is caller scratch (HP_IR_MAX_DUR entries) that
 * *out may point into. -HP_E_INVAL if `a` is not an IR action. */
int hp_map_ir_code(const hp_map_t *, const hp_action_t *, uint32_t *raw, hp_ir_code_t *out);

/* Runs one action through the sinks. IR: encode then one ir->send (via the output runtime when
 * attached: Sony protocols go out 3 times, RC5/RC6 toggle). BT: key down, key up. HA_EVENT / NONE:
 * no sink call. ACTIVITY is only valid from hp_key_handle (-HP_E_INVAL here). Returns 0 or a
 * negative error; -HP_E_INVAL if a needed sink is NULL. */
int hp_map_apply(const hp_map_t *, const hp_action_t *, const hp_sinks_t *);
/* Same for an externally requested one-off send (the ir_send verb): if the IR line is busy the
 * frame is deferred until it is free (see irout.h) instead of interleaving. */
int hp_map_apply_deferrable(const hp_map_t *, const hp_action_t *, const hp_sinks_t *);

size_t hp_map_activity_count(const hp_map_t *);
const char *hp_map_activity_name(const hp_map_t *, size_t index); /* NULL if out of range */
const char *hp_map_activity_id(const hp_map_t *, size_t index);
int hp_map_current(const hp_map_t *); /* -1 = none */

/* Runs the named activity's "steps" in order and makes it current. Returns
 * its index (>=0); -HP_E_INVAL for an unknown name; a step failure stops the
 * run, leaves the current activity unchanged and returns that error (steps
 * already sent are not undone). No implicit power-off of a previous activity. */
int hp_activity_set(hp_map_t *, const char *name, const hp_sinks_t *);
/* Runs the current activity's "off" steps and clears it. Returns the index of
 * the activity that was turned off; -HP_E_INVAL if none is current. */
int hp_activity_off(hp_map_t *, const hp_sinks_t *);

/* Incoming key from a remote: PRESS looks the key up in the current activity (then global) and
 * applies it (an "activity" action starts that activity / powers down; callers detect the change
 * through hp_map_current()); HOLD starts repeating the last IR frame sent for this key (a further HOLD for the same key
 * extends the safety cap, see irout.h); RELEASE stops it. `emit` (may be NULL) is always called afterwards so HA sees every key, mapped or not.
 * Returns the apply result (0 when nothing to apply). */
typedef void (*hp_key_emit_fn)(void *ctx, uint8_t remote, uint8_t key, hp_key_action_t action);
int hp_key_handle(hp_map_t *, const hp_sinks_t *, uint8_t remote, uint8_t key,
                  hp_key_action_t action, hp_key_emit_fn emit, void *ctx);

/* Cache (HA-offline fallback). File = "HPMC" | version u8 (1) | len u32 BE |
 * config text | CRC-32 (IEEE) u32 BE over everything before it, written with hp_atomic_write.
 * save: -HP_E_INVAL if no config is loaded, -HP_E_IO on I/O failure.
 * load: -HP_E_NOENT if missing, -HP_E_INVAL if corrupt/truncated/invalid
 * (map untouched), -HP_E_TOOBIG for an over-limit config, -HP_E_IO. */
int hp_map_save_cache(const hp_map_t *, const char *path);
int hp_map_load_cache(hp_map_t *, const char *path);

#endif
