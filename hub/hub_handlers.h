#ifndef HP_HUB_HANDLERS_H
#define HP_HUB_HANDLERS_H

/* Hub-side wiring of the engine to the WebSocket protocol.
 *
 * Message handling:
 *   set_config     -> hp_map_load; reply config_ack {ok, message?, config_hash?}. On success
 *                     the cache (if a path is set) is rewritten and
 *                     hello_ack.activities reflects the new config. On
 *                     failure ok=false + message and the old map is intact.
 *   command        -> command_result {ok, message?}. Vocabulary:
 *                       device "hub", command "start_activity", args {"name": "<activity>"}
 *                       device "hub", command "power_off"        (current activity's "off" steps)
 *                     The command_result only says the request was accepted. For format-2 configs
 *                     the transition then runs on timers (hub/engine/activity.h); when it completes
 *                     every authenticated client gets state_update {activity: <index>, power:
 *                     true|false} (id 0). Format 1 completes inside the command, so the
 *                     state_update follows the command_result at once. Per-device changes are
 *                     pushed as device_state {device, power, input?} (input omitted = unknown)
 *                     whenever the tracked state changes, and the full set after hello_ack.
 *                       device "hub", command "set_device_state",
 *                         args {"device": "<id>", "power"?: bool, "input"?: "<name>"|null}
 *                                   corrects the tracked state WITHOUT sending IR (a device was
 *                                   switched by hand); input null/"" = unknown. Unknown device or
 *                                   input -> command_result {ok:false, message}.
 *   hello_ack      -> activities = JSON array of activity names; the array
 *                     index is the `activity` value used in state_update.
 *   page_push      -> (ws, data = base64) packed with rf_payload, split at the
 *                     hub->remote frame cap (30) and sent over RF. Success:
 *                     command_result {ok:true} (same id). Failure: `error` with
 *                     code "too_big" (data > 255 bytes after decoding, or more
 *                     than 15 fragments; nothing is sent), "no_remote" (no
 *                     remote attached; nothing is sent) or "rf_error" (the link
 *                     failed mid-message). Always exactly one reply.
 *   command        -> more verbs (device "hub"):
 *                       ir_send     args = IR action object, same forms/validation as format-2
 *                                   config ({proto,addr,cmd} | {raw:{hz,us}} | {pronto:"hex"},
 *                                   optional port_mask); sends it once. Bad action ->
 *                                   command_result {ok:false, message}.
 *                       tile_update args {remote,page,tile,text} (u8,u8,u8,str<=255) -> forwarded
 *                                   to the remote over RF as tile_update. Replies like page_push
 *                                   (command_result ok / error no_remote | rf_error | too_big).
 *   hello_ack      -> also config_hash (8 lowercase hex chars, CRC-32 of the loaded config text;
 *                     omitted when no config is loaded).
 *   set_config     -> after the config_ack, every OTHER authenticated client gets
 *                     activity_list {activities:[names], config_hash?}; if the running activity
 *                     changed by the reload, everyone gets a state_update.
 *   state_update   -> also sent over RF (best effort) whenever the activity
 *                     changes, if a remote is attached.
 *   from RF        -> hub_pump_rf: frames (cap 32) are reassembled; key_event
 *                     -> hp_key_handle -> key_event broadcast (id 0); battery
 *                     -> battery broadcast {remote,percent,millivolts} (id 0).
 *                     Garbage frames / undecodable or unexpected messages are
 *                     dropped; a remote disconnecting resets reassembly.
 */

#include <stdbool.h>

#include "hub/engine/activity.h"
#include "hub/engine/irout.h"
#include "hub/engine/mapping.h"
#include "hub/engine/sched.h"
#include "hub/rf/framing.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/ws.h"

typedef struct {
    hp_map_t map;
    hp_sinks_t sinks;
    hp_ws_server_t *srv;
    const char *cache_path; /* NULL = do not persist */
    bool pending_state;     /* a state_update to broadcast after the reply */
    int pending_activity;
    bool pending_power;
    hp_rf_asm_t rf_asm;     /* remote->hub reassembly */
    uint8_t rf_seq;         /* hub->remote message sequence */
    bool pending_list;      /* an activity_list to broadcast after the reply */
    const hp_session_t *list_skip; /* ...to everyone but the set_config sender */
    hp_sched_t sched;       /* timers: key repeat, SIRC copies, activity transitions, state writes */
    hp_irout_t irout;
    hp_actsm_t act;         /* activity state machine (docs/proto/activity-state.md) */
    bool rf_attached;       /* last seen RF link state: announce remote_attached on a new attach */
    uint8_t pending_dev[HP_MAP_MAX_DEVICES]; /* devices whose device_state is to be broadcast, in change order */
    uint8_t npending_dev;
} hub_ctx_t;

/* Initialises the context with a CLOCK_MONOTONIC millisecond clock for the timers. */
void hub_ctx_init(hub_ctx_t *);
/* Replaces the timer clock (tests). Call before any timer is scheduled. */
void hub_set_clock(hub_ctx_t *, hp_clock_fn, void *);
/* Poll-loop helpers: ms until the next timer (-1 none), and run whatever is due (then broadcasts
 * whatever the timers changed). Never block. */
int hub_next_timeout_ms(const hub_ctx_t *);
void hub_run_timers(hub_ctx_t *);
/* Fills tbl->ctx and the handler/activities hooks (token etc. left to caller). */
void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx);
/* Sends the current state_update (id 0, the last COMPLETED activity) and then one device_state per
 * tracked device to one connection; used right after a successful hello so a (re)connecting client
 * learns the live state. No activity running -> {activity:0, power:false}. */
void hub_send_state(hub_ctx_t *, hp_ws_conn_t *);
/* Flushes the engine's queued activity_list / device_state / state_update broadcasts. */
void hub_flush_pending(hub_ctx_t *);
/* Clean shutdown: writes the state file if a change is still pending. */
void hub_shutdown(hub_ctx_t *);
/* Drains the RF transport, applying keys and broadcasting key_event. */
void hub_pump_rf(hub_ctx_t *);
/* Sends {v,id:0,t:type,d} to every authenticated connection; takes ownership of d. */
void hub_broadcast(hub_ctx_t *, const char *type, cJSON *d);

#endif
