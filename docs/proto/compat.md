# Protocol compatibility rules

The hub, the Home Assistant integration and the add-on are released separately, so any pairing of versions can meet.

- Changes are additive: new fields are optional; new message types may be added.
- Never renumber or reuse message ids or field meanings; never change a field's type.
- Receivers ignore fields they do not know, and log (debug) and ignore message types they do not know. Neither may fail a connection.
- Receivers still enforce required fields, types and ranges of the fields they know.
- Senders validate strictly against their own schema; only fields in the schema are sent.
- A new field must be optional in practice: everything has to work when a peer drops it.

## Keepalive and no-polling release (hub, add-on, integration)

Liveness is kernel TCP keepalive, not application timers: every accepted or opened socket in the chain sets `SO_KEEPALIVE`,
`TCP_KEEPIDLE=60`, `TCP_KEEPINTVL=10`, `TCP_KEEPCNT=3` (a dead peer is noticed in about 90 s).

- The hub advertises `tcp_keepalive: true` in `hello_ack`. The integration sends its 30 s `ping` only to a hub whose `hello_ack`
  lacks it (old firmware keeps the old 90 s silent-client close). A new hub never closes an authenticated client for silence.
- The integration and add-on talk over the push WebSocket `/api/v1/channel` (`docs/addon-api.md`); the poll routes are gone, with no
  fallback. An integration that gets `404` on the channel reports `addon_outdated` and raises a repair issue; an add-on that closes
  `4001` (hello `v` unsupported) makes it report `integration_outdated`.
- **Upgrade order:** hub firmware first (backward compatible: old integrations still ping), then the add-on, then the integration.
  Until the integration is updated an updated add-on has no consumer for bundles; until the add-on is updated an updated
  integration shows `addon_outdated`.
