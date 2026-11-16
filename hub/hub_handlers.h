#ifndef HP_HUB_HANDLERS_H
#define HP_HUB_HANDLERS_H

/* Hub-side wiring of the engine to the WebSocket protocol.
 *
 * Message handling:
 *   set_config     -> hp_map_load; reply config_ack {ok, message?}. On success
 *                     the cache (if a path is set) is rewritten and
 *                     hello_ack.activities reflects the new config. On
 *                     failure ok=false + message and the old map is intact.
 *   command        -> command_result {ok, message?}. Vocabulary:
 *                       device "hub", command "start_activity", args {"name": "<activity>"}
 *                       device "hub", command "power_off"        (current activity's "off" steps)
 *                     start_activity / power_off broadcast state_update
 *                     {activity: <index>, power: true|false} (id 0) to every
 *                     authenticated client AFTER the command_result.
 *   hello_ack      -> activities = JSON array of activity names; the array
 *                     index is the `activity` value used in state_update.
 *   page_push      -> (ws, data = base64) packed with rf_payload, split at the
 *                     hub->remote frame cap (30) and sent over RF. Success:
 *                     command_result {ok:true} (same id). Failure: `error` with
 *                     code "too_big" (data > 255 bytes after decoding, or more
 *                     than 15 fragments; nothing is sent), "no_remote" (no
 *                     remote attached; nothing is sent) or "rf_error" (the link
 *                     failed mid-message). Always exactly one reply.
 *   state_update   -> also sent over RF (best effort) whenever the activity
 *                     changes, if a remote is attached.
 *   from RF        -> hub_pump_rf: frames (cap 32) are reassembled; key_event
 *                     -> hp_key_handle -> key_event broadcast (id 0); battery
 *                     -> battery broadcast {remote,percent,millivolts} (id 0).
 *                     Garbage frames / undecodable or unexpected messages are
 *                     dropped; a remote disconnecting resets reassembly.
 */

#include <stdbool.h>

#include "hub/engine/mapping.h"
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
} hub_ctx_t;

void hub_ctx_init(hub_ctx_t *);
/* Fills tbl->ctx and the handler/activities hooks (token etc. left to caller). */
void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx);
/* Sends the current state_update (id 0) to one connection; used right after a successful hello so
 * a (re)connecting client learns the live state. No activity running -> {activity:0, power:false}. */
void hub_send_state(hub_ctx_t *, hp_ws_conn_t *);
/* Sends any queued state_update to all authenticated clients. */
void hub_flush_pending(hub_ctx_t *);
/* Drains the RF transport, applying keys and broadcasting key_event. */
void hub_pump_rf(hub_ctx_t *);
/* Sends {v,id:0,t:type,d} to every authenticated connection; takes ownership of d. */
void hub_broadcast(hub_ctx_t *, const char *type, cJSON *d);

#endif
