# Hub log shipping

The hub never stores logs. Every line the daemon writes to stderr goes (a) straight through to the tmpfs log
(`/var/volatile/ha-harmony.log`) and (b) into a 64-line in-memory ring (192 bytes per line, longer lines are cut).

- `hub_log` (code 41, hub -> client, ws, id 0): `level: "info"|"warning"` (warning = the line says cannot / failed / error / giving up / dropping / refus / unusable), `msg: str`, `ts: u32` (hub uptime ms, wraps), `dropped: u32?` (lines the ring lost before this one).
- Sent only to authenticated clients whose `hello.client` is `home-assistant` (others, such as tools and tests, never see it), the moment the line is produced (the poll loop wakes on a pipe, no timer). While no such client is connected the ring holds the lines, oldest dropped first, and flushes in order right after the next `hello_ack` and state push.
- The integration logs each line under `custom_components.ha_harmony.hub_log` (`[hub id] msg`; INFO, or WARNING for `warning`, so those show in HA's default log) and forwards it to the add-on as a `hub_log` channel frame (`docs/addon-api.md`), which prints it to the add-on log as `hub.<id>`.
- Lines written before the daemon's argument check (usage errors) only reach the tmpfs log.
