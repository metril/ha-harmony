#ifndef HP_NET_JSONRPC_H
#define HP_NET_JSONRPC_H

/* Schema-validating envelope dispatcher: {"v":1,"id":<u32>,"t":"<name>","d":{...}}.
 *
 * Auth state is per connection and is carried in `hp_session_t`: the caller
 * allocates one per WebSocket connection (zero-initialised) and passes it as
 * the `user` argument of hp_dispatch. Shared (per-hub) state lives in the
 * handler table. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/gen/harmony_proto.h"
#include "vendor/cjson/cJSON.h"

typedef struct {
    bool authed; /* set by a successful hello */
    bool bt_listen; /* bt_listen on: wants bt_remote_key pushes; reset by hello */
} hp_session_t;

/* Called for a validated, authenticated message. `d` is the (possibly empty)
 * data object, valid only during the call. Write a complete reply envelope
 * into `reply` (use hp_make_reply / hp_make_error) and set *reply_len, or
 * leave *reply_len at 0 for no reply. Return 0, or a negative -HP_E_* code
 * (nothing is sent and hp_dispatch returns it). */
typedef int (*hp_handler_fn)(void *ctx, hp_session_t *sess, uint32_t id, const cJSON *d,
                             char *reply, size_t reply_cap, size_t *reply_len);

typedef struct {
    const char *token;       /* expected hello token; NULL/empty rejects every hello */
    const char *hub_id;      /* reported in hello_ack */
    const char *fw_version;  /* optional, reported in hello_ack */
    void *ctx;               /* passed to handlers */
    /* Optional: returns a new cJSON value (caller-owned, handed to hello_ack)
     * for hello_ack.activities, or NULL to omit the field. */
    cJSON *(*activities)(void *ctx);
    /* Optional: writes the loaded config's hash (hex, NUL-terminated, <= 8 chars) into out[9] and
     * returns true for hello_ack.config_hash; false/NULL omits the field. */
    bool (*config_hash)(void *ctx, char out[9]);
    /* Optional: a new cJSON object (caller-owned, handed to hello_ack) for hello_ack.limits. */
    cJSON *(*limits)(void *ctx);
    /* Optional: adds more optional hello_ack fields (cJSON_Add* on `ack`). */
    void (*hello_extra)(void *ctx, cJSON *ack);
    /* Indexed by hp_msg_type_t. hello is handled internally; ping has a
     * built-in pong when handler[HP_MSG_PING] is NULL. NULL otherwise =>
     * error/unknown_type. */
    hp_handler_fn handler[HP_MSG_MAX_CODE + 1];
} hp_handler_tbl_t;

#define HP_DISPATCH_OK 0
#define HP_DISPATCH_CLOSE 1 /* reply (if any) must be sent, then the connection closed */

/* Validates and dispatches one text message. `user` must be an hp_session_t*.
 * Returns HP_DISPATCH_OK, HP_DISPATCH_CLOSE, or -HP_E_INVAL (bad args) /
 * -HP_E_TOOBIG (reply buffer too small). Protocol errors are not failures:
 * they produce an `error` envelope in `reply` (id echoed, 0 if unparseable)
 * with code unknown_type | version | bad_request | unauthorized. */
int hp_dispatch(const char *json, size_t len, const hp_handler_tbl_t *tbl, void *user,
                char *reply, size_t reply_cap, size_t *reply_len);

/* Reply builders for handlers. hp_make_reply takes ownership of `d` (may be NULL). */
int hp_make_reply(uint32_t id, const char *type, cJSON *d, char *reply, size_t cap, size_t *len);
int hp_make_error(uint32_t id, const char *code, const char *message, char *reply, size_t cap,
                  size_t *len);

#endif
