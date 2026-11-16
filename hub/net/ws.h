#ifndef HP_NET_WS_H
#define HP_NET_WS_H

/* Minimal RFC 6455 server: poll(2), single thread, text frames only.
 * Functions return 0/positive on success and -HP_E_* (hub/hp_errors.h) on
 * failure unless noted. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_WS_MAX_FRAME 65536   /* max payload of one frame and of a reassembled message */
#define HP_WS_MAX_CONNS 16
#define HP_WS_MAX_OUT (256 * 1024)  /* per-connection unsent-bytes cap; exceeded => drop */
#define HP_WS_HANDSHAKE_MS 5000     /* time allowed to finish the HTTP upgrade */
#define HP_WS_AUTH_MS 10000         /* upgraded conn must be marked authed within this (default) */
#define HP_WS_IDLE_MS 90000         /* max silence (no inbound frame of any kind) (default) */
#define HP_WS_CLOSE_MS 2000         /* time allowed to flush a close frame */

typedef struct hp_ws_server hp_ws_server_t;
typedef struct hp_ws_conn hp_ws_conn_t;

typedef struct {
    /* Complete, UTF-8-validated text message (not NUL-terminated). */
    void (*on_text)(hp_ws_conn_t *, const char *, size_t, void *user);
    /* Called once for every upgraded connection that ends, before it is freed. */
    void (*on_close)(hp_ws_conn_t *, void *user);
} hp_ws_cb_t;

/* Binds 0.0.0.0:port (0 = ephemeral). `token` is stored for the owner (see
 * hp_ws_token); authentication itself is in-band (hello, jsonrpc.h). Returns
 * NULL on failure. `cb` is copied. Ignores SIGPIPE; all sends use MSG_NOSIGNAL. */
hp_ws_server_t *hp_ws_listen(uint16_t port, const char *token, const hp_ws_cb_t *cb, void *user);
uint16_t hp_ws_port(const hp_ws_server_t *);
const char *hp_ws_token(const hp_ws_server_t *);
/* Runs one poll iteration (accept, read, dispatch, flush, timeouts). Returns
 * the number of ready fds (>=0, 0 on timeout/EINTR) or -HP_E_INVAL. */
int hp_ws_poll(hp_ws_server_t *, int timeout_ms);
/* Queues one text frame. -HP_E_TOOBIG if len > HP_WS_MAX_FRAME, -HP_E_BUSY if
 * the peer is too slow (connection is dropped) or already closing. */
int hp_ws_send(hp_ws_conn_t *, const char *text, size_t len);
/* Sends a close frame with `code` (e.g. 1008) and closes once flushed. */
void hp_ws_conn_close(hp_ws_conn_t *, uint16_t code);
/* Overrides the auth/idle deadlines (ms, >0) for connections upgraded from now on. */
int hp_ws_set_timeouts(hp_ws_server_t *, int auth_ms, int idle_ms);
/* The owner calls this once the peer authenticated (hello ok); until then an
 * upgraded connection is closed (1008) after the auth deadline. */
void hp_ws_conn_set_authed(hp_ws_conn_t *);
/* Calls fn for every upgraded, open connection (e.g. to broadcast). fn must
 * not close connections other than via hp_ws_conn_close/hp_ws_send. */
void hp_ws_foreach(hp_ws_server_t *, void (*fn)(hp_ws_conn_t *, void *), void *arg);
/* Optional: fn(arg, out) fills up to 4 extra fds polled for readability each
 * hp_ws_poll; they only wake the poll (the owner services them). */
void hp_ws_set_extra_fds(hp_ws_server_t *, int (*fn)(void *arg, int out[4]), void *arg);
/* Per-connection opaque pointer (owner frees it in on_close). */
void hp_ws_conn_set_user(hp_ws_conn_t *, void *);
void *hp_ws_conn_user(const hp_ws_conn_t *);
/* Closes all connections (calling on_close) and the listener. NULL-safe. */
void hp_ws_close(hp_ws_server_t *);

/* ---- pure helpers, exposed for tests ---- */
void hp_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
/* Writes len4/3 rounded up *4 chars + NUL; returns chars written. out must hold 4*((len+2)/3)+1. */
size_t hp_base64_encode(const uint8_t *in, size_t len, char *out);
/* Decodes strictly valid base64; returns the byte count, or (size_t)-1 if invalid or > cap. */
size_t hp_base64_decode(const char *s, size_t len, uint8_t *out, size_t cap);
/* Strict base64 (padded, standard alphabet) validity check. */
bool hp_base64_valid(const char *s, size_t len);
/* out must hold 29 bytes. */
void hp_ws_accept_key(const char *key, size_t key_len, char out[29]);
/* Strict UTF-8 (no overlongs, surrogates, > U+10FFFF). */
bool hp_utf8_valid(const uint8_t *s, size_t len);

#define HP_WS_HS_NEED_MORE 1
#define HP_WS_HS_OK 0
/* Parses an HTTP upgrade request in buf. OK: *consumed = request length and
 * accept[29] is filled. NEED_MORE: no blank line yet. Negative: -400 bad
 * request, -426 bad version, -431 header block too large. */
int hp_ws_parse_handshake(const char *buf, size_t len, size_t *consumed, char accept[29]);

typedef struct {
    bool fin;
    uint8_t opcode;
    uint8_t mask[4];
    uint64_t payload_len;
    size_t hdr_len;
} hp_ws_frame_hdr_t;

#define HP_WS_FRAME_NEED_MORE 1
#define HP_WS_FRAME_OK 0
/* Parses a client frame header. Negative result is -(close code): -1002
 * protocol error (unmasked, RSV bits, bad opcode, bad control frame), -1009
 * too big (payload_len > HP_WS_MAX_FRAME, including 64-bit lengths). */
int hp_ws_parse_frame_hdr(const uint8_t *buf, size_t len, hp_ws_frame_hdr_t *h);

#endif
