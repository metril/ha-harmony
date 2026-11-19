# Bench gate: Bluetooth HID

Run on the real hub after flashing a firmware with the Bluetooth helper (`hub bt-helper`, supervised by `hub/bt_link.c`).
Design and wire format: `docs/proto/bluetooth.md`, `hub/bt/bt_ipc.h`; hardware facts: `docs/hardware/hub-bluetooth.md`.
Use delegated bench operation; the unit tests never touch a radio, so everything below is first contact. Never run the helper
by hand on a development machine without `--hci N` pointing at a spare controller.

Setup: a config with one `bt_hosts` entry per target (address filled in after pairing) and one `bt_remotes` entry (Bluetooth LE
remote), `kind: bt` devices whose commands use built-in names and `HID:<page>:<usage>`. Watch the daemon log and `bt_state`
(`bt_listen` on a ws client shows keys from remotes). Note the time and the numbers requested below.

## Checklist (tick each; write the measurement next to it)

### Pairing and keys
- [ ] **Pair each host class**: Nvidia Shield, Apple TV, Fire TV Classic, over Classic and over LE where the host supports both.
      `bt_pair` host mode, pair from the host; `pairing.result: ok` with the right transport; the host lists the hub as a keyboard.
- [ ] **Key latency while connected** (`bt_key` press, or an IR-remote press mapped to a bt action, to the host acting on it):
      under 150 ms, median and worst of 20 presses per host.
- [ ] **Reconnect**: power-cycle the host or walk out of range and back: the next key reaches it within 3 s of the key
      (`CONNECT` is sent when the activity starts, so also time activity start to first usable key).
- [ ] **Simultaneous LE central and peripheral**: a bonded LE remote typing into the hub while an LE host is connected and
      receiving keys; neither stalls (hold a key on the remote and send `bt_key` presses at the same time).
- [ ] **Real `conn_max`**: connect hosts one by one until the controller refuses; compare with `hello_ack.bt.conn_max` (the
      helper's constant); record the real number and whether LE and Classic share it.

### Supervision must never touch IR, RF or the boot health
- [ ] **`kill -9` the helper** (`pkill -9 -f 'hub bt-helper'`): IR and RF keep working throughout; `bt_state.state` becomes
      `off`, hosts/remotes empty; a key held on a host is released; the helper is back after about 5 s (30 s, then 120 s after
      repeats); the health mark is untouched, no reboot, no boot-strike count change (`tools/boot` log).
- [ ] **`SIGSTOP` the helper**: the hub notices within 15 s of silence and replaces it; IR/RF unaffected; same checks as above.
- [ ] **Five restarts**: kill it six times in a row: after the last the state is `disabled` and stays so; the hub keeps running;
      a hub restart brings it back.
- [ ] **Stock fallback**: with the helper never started or the hub binary replaced by the stock firmware path, reboot: stock
      `bluetoothd` is running again (`pgrep bluetoothd`); nothing on disk was changed by us. With Bluetooth in the config,
      `bluetoothd` is stopped once before the first helper start and not touched again that boot.
- [ ] **Config gating**: a config with no bt hosts or remotes never starts the helper (no `hub bt-helper` process, bluetoothd
      untouched); removing them stops it (SIGTERM) without counting a failure.

### Resources
- [ ] **VmPeak vs RLIMIT_AS**: `grep VmPeak /proc/<helper pid>/status` after a pairing session with the most bonds you intend
      to support, against the helper's 32 MiB address-space cap (`hub/bt/bt_main.c`). Tighten or raise the cap with margin;
      record RSS too. Also the hub's own RSS before and after.
- [ ] **LE bond count vs reconnect time**: with 4, 8, 16 and 32 LE bonds (hosts + remotes), time a reconnect and the helper's
      start-up to ready; record where it stops being acceptable (feeds `hosts_max` / `remotes_max` / `le_bonds_max`).

### Remote pairing flow
- [ ] **Candidate flow**: `bt_pair {mode: "remote"}` lists advertising remotes as `candidates` (max 16, names arrive later,
      RSSI-only changes do not push); picking one with `addr` bonds exactly that one; a listed remote that went away answers
      `no_candidate`; nothing is ever bonded without a pick.
- [ ] **Bond eviction**: fill the bond table, pair one more: the oldest or least-used bond is evicted as designed and the editor
      sees the change (`bt_state`); a configured remote is not silently dropped.
- [ ] **Unidentified central**: a central that connects and never identifies itself (no address resolution, no HID discovery) is
      dropped by the helper after its timeout; it never occupies a host slot and never appears in `hosts`.
- [ ] **HIDS-gated pairing**: a remote that does not expose HID-over-GATT is not offered or bonded; a keyboard/consumer-control
      remote is.
- [ ] **Backoff of a missing remote**: a bonded remote that is off: reconnect attempts back off (the `fails` counter in
      REMOTES, up to 5 minutes) without load on the controller; it reconnects within the backoff after power-on.
- [ ] **Configured vs unconfigured**: a bonded remote/host that is not in the config shows `configured: false` with a free slot
      number; add it to the config by address: the entry flips to `configured: true` on its config slot and its keys reach
      the key mapping; a configured host whose address is not bonded answers `bt_key` with `no_slot`.
- [ ] **Pairing window**: `window` is present exactly while a window is open (host and remote), absent after the helper
      reports it closed; the helper lingers 60 s after a window with no config entries, then stops.

### Failure paths
- [ ] **HCI fatal path**: provoke a fatal controller error (unplug/reset the UART or `hciconfig hci0 down` under the helper):
      the helper reports `error`/`no_adapter` and exits; the hub shows `no_adapter`, retries on the backoff schedule, ends
      `disabled` at the cap; IR and RF are never affected; after a hub reboot it works again.
- [ ] **Slow or stuck host**: a host that stops reading: keys are dropped (never queued behind it, never blocking the hub);
      the hub event loop keeps answering websocket pings; other hosts still receive keys.
- [ ] **Flood**: mash a Bluetooth remote's keys while IR/RF keys are pressed: no IR/RF key is delayed or lost.

### Radio setup
- [ ] **CSR vendor settings survive BTstack's HCI reset**: stock `rcS` writes three vendor `hcitool cmd` settings after
      `hciconfig hci0 up`; BTstack resets the controller when it starts. After the helper is up, read back the same settings
      (same `hcitool cmd` reads as the stock script writes) and compare; if they reverted (range, power, sniff/LE
      parameters), the helper has to re-apply them after its reset (record the values, which ones changed).
- [ ] Range and throughput with the helper's changed settings against the stock firmware: same or better.

## Record

Per item: date, firmware build id, numbers, pass/fail, and what was changed. Anything that changes a limit
(`hosts_max`, `conn_max`, `le_bonds_max`, the address-space cap, the supervision timings in `hub/bt_link_sup.h`) is
a code change with a test, not a bench-only tweak.
