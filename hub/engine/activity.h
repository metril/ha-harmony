#ifndef HP_ENGINE_ACTIVITY_H
#define HP_ENGINE_ACTIVITY_H

/* Hub-executed activities (config format 2): per-device power/input tracking and a non-blocking
 * transition planner on the hp_sched queue, so activities work while Home Assistant is down.
 * Format 1 (flat steps, no devices) keeps the immediate hp_activity_set/off behaviour.
 *
 * Transition = a plan of timed IR sends, built from the TRACKED state when the request arrives:
 *   start B:  1. power OFF every device that is on and not used by B
 *             2. power ON every B device that is off (never a toggle to a device already in the
 *                wanted state; devices without power codes are only tracked)
 *             3. input select, in device order, where the tracked input differs or is unknown;
 *                it waits until the device's settle_ms (from its power-on / last input) elapsed
 *             4. B's extra `steps` (wait for all settle times)
 *   off:      1. power OFF everything that is on   2. the current activity's `off` steps
 * Powering a device off marks its input unknown. Every send updates the tracked state when it
 * goes out; steps re-check the tracked state at execution time (a set_device_state correction
 * during a transition skips what became redundant).
 *
 * Sequencing: one step per timer; the next step is not before (previous frame time incl. SIRC
 * copies + HP_ACT_GUARD_MS), so frames never overlap. The bus-free time persists across plans.
 *
 * Interruption rule: a new start/off while a transition runs cancels the remaining plan steps
 * (frames already sent stay applied in the tracked state), then plans from the tracked state.
 * Pending settle times survive, so a half-powered device is not sent an input too early.
 * IR key presses and the ir_send / device_command verbs that arrive while busy are queued (small FIFOs) and
 * run when the transition finishes; they are dropped (logged) when it is cancelled or aborted. BT keys always go out.
 *
 * Reporting: the current activity (map->current) switches to the target when the request is
 * accepted (key lookup uses it); the REPORTED activity (state_update, hello state) changes only
 * when the transition completes. device_state is reported whenever a device's tracked state
 * changes (each send, set_device_state, reload). A failed send aborts the plan and reverts the
 * current activity to the reported one.
 *
 * Persistence (--state): see hp_actsm_save_state. Writes are debounced to one per
 * HP_ACT_PERSIST_MS; hp_actsm_flush_state forces one (clean shutdown). */

#include <stdbool.h>
#include <stdint.h>

#include "hub/engine/mapping.h"
#include "hub/engine/sched.h"

#define HP_ACT_GUARD_MS 50
#define HP_ACT_PERSIST_MS 2000
#define HP_ACT_STATE_VER 1
#define HP_ACT_PLAN_MAX (HP_MAP_MAX_DEVICES + 2 * HP_MAP_MAX_ACT_DEVS + 2 * HP_MAP_MAX_STEPS + 8)

typedef enum { HP_ACTSM_EVT_DEVICE = 1, HP_ACTSM_EVT_ACTIVITY = 2, HP_ACTSM_EVT_START = 3 /* idx = the activity being started */ } hp_actsm_evt_t;
/* DEVICE: idx = device index; ACTIVITY: idx = reported activity (-1 none). */
typedef void (*hp_actsm_notify_fn)(void *ctx, hp_actsm_evt_t evt, int idx);

typedef enum { HP_STEP_PWR_OFF = 1, HP_STEP_PWR_ON, HP_STEP_INPUT, HP_STEP_EXTRA, HP_STEP_OFFX } hp_stepkind_t;

typedef struct {
    uint8_t kind, dev;
    uint8_t force;          /* resend a discrete power / input code whatever the tracked state says */
    int32_t input;          /* HP_STEP_INPUT: index into map.inputs */
    const hp_action_t *act; /* EXTRA/OFFX action */
} hp_actstep_t;

#define HP_ACT_CMDQ_MAX 8
/* A client request that arrived mid-transition (or must wait for a device to settle): run when the line is free. */
typedef struct {
    uint8_t kind;  /* 1 device_command, 2 ir_send (text = the args JSON, owned) */
    bool nopre;    /* an input command already preceded by its power-on */
    char dev[HP_MAP_NAME_MAX];
    char cmd[HP_MAP_NAME_MAX + 8];
    char *text;
} hp_actcmd_t;

/* A device command whose frame waits in the IR queue: its tracked-state change applies when it is sent. */
typedef struct {
    struct hp_actsm *e;
    bool used, has_input, settle;
    int8_t power; /* -1 keep */
    char dev[HP_MAP_NAME_MAX];
    char input[HP_MAP_NAME_MAX];
} hp_actpc_t;

typedef struct {
    char id[HP_MAP_NAME_MAX];
    uint8_t power;
    char input[HP_MAP_NAME_MAX]; /* "" = unknown */
    bool settling;
    uint32_t settle_until;
} hp_devstate_t;

typedef struct hp_actsm {
    hp_map_t *map;
    hp_sched_t *sched;
    const hp_sinks_t *sinks;
    hp_actsm_notify_fn notify;
    void *notify_ctx;
    hp_devstate_t dev[HP_MAP_MAX_DEVICES];
    uint8_t ndev;
    int reported; /* last completed activity, -1 none */
    char rep_key[HP_MAP_NAME_MAX]; /* its identity (id; name in format 1), to re-find it on reload */
    hp_actstep_t plan[HP_ACT_PLAN_MAX];
    uint16_t nplan, pos;
    bool running;
    bool resync; /* plan of a re-pressed current activity: discrete steps ignore the tracked state */
    int target;   /* activity being started, -1 = off */
    char tgt_key[HP_MAP_NAME_MAX];      /* its identity ("" = off) and, for off, the activity whose off steps run: to restart the plan after a reload */
    char prev_key[HP_MAP_NAME_MAX];
    char last_key[HP_MAP_NAME_MAX];     /* last activity that was reported (survives Off): whose devices a stale Off still powers down */
    uint32_t loaded_hash;               /* config hash the device tracking was last keyed to */
    bool have_hash;
    hp_actcmd_t cq[HP_ACT_CMDQ_MAX];
    uint8_t cq_n;
    hp_sched_handle cq_h;
    bool draining;
    void (*irsend_fn)(void *ctx, const char *json);
    void *irsend_ctx;
    hp_actpc_t pc[HP_IROUT_PEND];
    uint8_t npc;
    hp_sched_handle timer;
    bool bus_busy;
    uint32_t bus_free, bus_len; /* deadline + the span it was set with (see remaining()) */
    const char *state_path;
    bool dirty, wrote;
    uint32_t saved_hash; /* config hash of the last state written/restored */
    uint32_t last_write;
    hp_sched_handle persist_h;
    unsigned writes; /* state file writes so far (tests/diagnostics) */
} hp_actsm_t;

void hp_actsm_init(hp_actsm_t *, hp_map_t *, hp_sched_t *, const hp_sinks_t *, hp_actsm_notify_fn, void *);
/* Call after every successful hp_map_load: does nothing when the config hash is unchanged (a running
 * transition keeps going; its plan only re-points at the reloaded tables). Otherwise re-keys the tracked
 * device state by device id, restarts a running transition's plan for its target when that survives (else
 * cancels it) (new devices start off/unknown), re-resolves the last COMPLETED
 * activity in the new map by id (name in format 1; none if gone; a transition cut short by the
 * reload is never reported, even though map->current carried its target over) and notifies every
 * device (and the activity if it changed). */
void hp_actsm_reloaded(hp_actsm_t *);

/* Start the named activity / a power-down. Return the activity index (off: the activity that was
 * current, 0 when none), -HP_E_INVAL (unknown activity), or the error of a synchronous first send.
 * Off is never a no-op: nothing tracked on still resends the discrete power-off codes of the
 * current / last activity's devices (all devices when none is known). */
int hp_actsm_start(hp_actsm_t *, const char *name);
int hp_actsm_start_idx(hp_actsm_t *, int idx);
int hp_actsm_off(hp_actsm_t *);
bool hp_actsm_busy(const hp_actsm_t *);
/* Reported (last completed) activity, -1 none. */
int hp_actsm_reported(const hp_actsm_t *);

/* Correct the tracked state without sending IR. power: -1 keep, 0/1. input: NULL keep, ""
 * unknown, else an input name of that device. -HP_E_INVAL for an unknown device/input. */
int hp_actsm_set_device(hp_actsm_t *, const char *device, int power, const char *input);
/* Sends a device command of the config through the deferrable (line-owner) path and updates the
 * tracked state itself. command: a YAML command (no state change), "power_on"/"power_off" (a
 * toggle device is sent NOTHING when already in that state; discrete codes are always sent) or
 * "input_<name>" (also marks the device on). -HP_E_NOENT unknown device, -HP_E_INVAL unknown
 * command/input or power without power codes, -HP_E_BUSY while a transition runs, else the
 * send error (state untouched). */
int hp_actsm_device_command(hp_actsm_t *, const char *device, const char *command);
/* Queues an ir_send (its args as JSON, malloc'd; owned on success) to run when the current transition
 * finishes, through the callback set below. 0, or -HP_E_BUSY when the queue is full (not owned then). */
int hp_actsm_defer_irsend(hp_actsm_t *, char *json);
void hp_actsm_set_irsend(hp_actsm_t *, void (*fn)(void *ctx, const char *json), void *ctx);
const hp_devstate_t *hp_actsm_device(const hp_actsm_t *, size_t idx); /* NULL if out of range */

/* State file: "HPST" | ver u8 | config hash u32 BE | activity id (u8 len + bytes, len 0 = none)
 * | ndev u8 | per device: id (u8 len + bytes), power u8, input (u8 len + bytes) | CRC-32 u32 BE
 * over everything before. Written with hp_atomic_write. */
void hp_actsm_set_state_path(hp_actsm_t *, const char *path);
/* Restores from the file per device / activity id (ids the config no longer has are dropped, whatever
 * the config hash); a missing/corrupt/other-version file leaves the state as is (all off/unknown) and
 * returns a negative error (-HP_E_NOENT missing, -HP_E_INVAL otherwise). */
int hp_actsm_load_state(hp_actsm_t *);
/* Writes the state file now (no-op without a path). 0 or -HP_E_IO. */
int hp_actsm_flush_state(hp_actsm_t *);

#endif
