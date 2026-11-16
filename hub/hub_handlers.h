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
 *                       device "sim", command "inject_key", args {"remote":u8,"key":u8,"action":0..2}
 *                         (sim builds only: queues a scripted RF frame, handled by
 *                          hub_pump_rf like a real remote key)
 *                     start_activity / power_off broadcast state_update
 *                     {activity: <index>, power: true|false} (id 0) to every
 *                     authenticated client AFTER the command_result.
 *   hello_ack      -> activities = JSON array of activity names; the array
 *                     index is the `activity` value used in state_update.
 *   key from RF    -> hub_pump_rf: scripted/real RF frame {remote,key,action}
 *                     -> hp_key_handle -> key_event broadcast (id 0).
 * Scripted RF frame (sim stub; Task 5 replaces with the real RF codec):
 *   3 bytes: remote u8, key u8, action u8 (hp_key_action_t).
 */

#include <stdbool.h>

#include "hub/engine/mapping.h"
#include "hub/io/sim_sinks.h"
#include "hub/net/jsonrpc.h"
#include "hub/net/ws.h"

typedef struct {
    hp_map_t map;
    hp_sinks_t sinks;
    hp_sim_t *sim;          /* non-NULL only in sim builds (enables device "sim") */
    hp_ws_server_t *srv;
    const char *cache_path; /* NULL = do not persist */
    bool pending_state;     /* a state_update to broadcast after the reply */
    int pending_activity;
    bool pending_power;
} hub_ctx_t;

void hub_ctx_init(hub_ctx_t *);
/* Fills tbl->ctx and the handler/activities hooks (token etc. left to caller). */
void hub_install(hp_handler_tbl_t *tbl, hub_ctx_t *ctx);
/* Sends any queued state_update to all authenticated clients. */
void hub_flush_pending(hub_ctx_t *);
/* Drains the RF transport, applying keys and broadcasting key_event. */
void hub_pump_rf(hub_ctx_t *);
/* Sends {v,id:0,t:type,d} to every authenticated connection; takes ownership of d. */
void hub_broadcast(hub_ctx_t *, const char *type, cJSON *d);

#endif
