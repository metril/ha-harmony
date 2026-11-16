#ifndef HP_IO_RF_PAIR_H
#define HP_IO_RF_PAIR_H

/* Cross-process simulated RF link: a Unix SOCK_SEQPACKET socket (one datagram
 * = one RF frame). The hub side binds and listens; the remote connects.
 * Both ends are hp_rf_transport vtables (hub/io/sinks.h).
 *
 * Single-owner: claim() is 0 once, then -HP_E_BUSY. Only ONE remote may be
 * attached to a hub: while one is, further connections are accepted and
 * closed at once (the connector's wait_attached() then reports -HP_E_BUSY).
 * A connection is only accepted for attachment after the previous peer's
 * queued frames have been read and its EOF seen.
 *
 * Handshake: on attaching a remote the hub sends the 1-byte frame 'A'.
 *
 * Hub-side semantics: send() with no attached remote -> -HP_E_NOENT;
 * recv() returns frame length, 0 on timeout, and -HP_E_NOENT exactly once
 * when the attached remote disconnects (callers reset reassembly then).
 * Remote-side recv() returns -HP_E_IO once the hub is gone.
 * Frames longer than the 32-byte RF maximum are -HP_E_TOOBIG on send. */

#include <stdbool.h>

#include "hub/io/sinks.h"

/* Hub side: bind+listen at path (an existing stale file is unlinked). */
int hp_rf_pair_create(const char *path, hp_rf_transport **out);
/* Remote side: connect to a hub's path. Does not wait for the handshake. */
int hp_rf_pair_connect(const char *path, hp_rf_transport **out);
/* Remote side: waits for the hub's handshake. 0 attached, -HP_E_BUSY refused
 * (another remote is attached), -HP_E_IO hub gone, 0-timeout -> -HP_E_IO too. */
int hp_rf_pair_wait_attached(hp_rf_transport *, int timeout_ms);
/* Remote side drains unread frames first (a reset would drop frames it sent).
 * Closes sockets (and unlinks the path on the hub side) and frees. NULL-safe. */
void hp_rf_pair_destroy(hp_rf_transport *);
/* Hub side: fds to poll for readability (listener, attached peer); returns count (0..2). */
int hp_rf_pair_fds(const hp_rf_transport *, int out[2]);
/* Hub side: is a remote attached (after servicing pending connections)? */
bool hp_rf_pair_connected(hp_rf_transport *);

#endif
