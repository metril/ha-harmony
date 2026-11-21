# Hub log shipping

The hub never stores logs. Every line the daemon writes to stderr goes (a) straight through to the tmpfs log
(`/var/volatile/ha-harmony.log`) and (b) into a 64-line in-memory ring (192 bytes per line, longer lines are cut).

- `hub_log` (code 41, hub -> client, ws, id 0): `level: "info"|"warning"` (warning = the line says cannot / failed / error / giving up / dropping / refus / unusable), `msg: str`, `ts: u32` (hub uptime ms, wraps), `dropped: u32?` (lines the ring lost before this one).
- Sent only to authenticated clients whose `hello.client` is `home-assistant` (others, such as tools and tests, never see it), the moment the line is produced (the poll loop wakes on a pipe, no timer). While no such client is connected the ring holds the lines, oldest dropped first, and flushes in order right after the next `hello_ack` and state push.
- The integration logs each line under `custom_components.ha_harmony.hub_log` (`[hub id] msg`; INFO, or WARNING for `warning`, so those show in HA's default log) and forwards it to the add-on as a `hub_log` channel frame (`docs/addon-api.md`), which prints it to the add-on log as `hub.<id>`.
- Lines written before the daemon's argument check (usage errors) only reach the tmpfs log.
- Capture is `hp_log()` (hub/net/hublog.h), not a redirect of fd 2: each line is `write()`n to fd 2 first (crash-safe) and try-locked into the ring; a busy lock or full ring drops the line from the ring and counts it (`dropped`). Shipping is rate limited by a token bucket (burst 30, 10 lines/s); suppressed lines still reach the file and one `hub: N log lines suppressed` line is shipped when the flow resumes. The Bluetooth helper's stderr is a separate `O_NONBLOCK` pipe polled by the main loop (a stall drops bytes, never blocks the helper).
- The boot hook logs to tmpfs `/var/volatile/ha-harmony-hook.log` (never flash; the installer removes the old `/cache/ha-harmony.log`). The daemon reads it at start (it holds respawn notes written while the daemon was down) and follows it with inotify, shipping each line once as `[boot-hook] ...` (offset kept in memory; a respawned daemon re-sends the file once).
