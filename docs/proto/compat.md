# Protocol compatibility rules

The hub, the Home Assistant integration and the add-on are released separately, so any pairing of versions can meet.

- Changes are additive: new fields are optional; new message types may be added.
- Never renumber or reuse message ids or field meanings; never change a field's type.
- Receivers ignore fields they do not know, and log (debug) and ignore message types they do not know. Neither may fail a connection.
- Receivers still enforce required fields, types and ranges of the fields they know.
- Senders validate strictly against their own schema; only fields in the schema are sent.
- A new field must be optional in practice: everything has to work when a peer drops it.
