# Phase 4 runbook: hub boot persistence

Plan and rationale: `docs/plans/2026-11-17-phase4-persistence.md`. All commands run from the repo root.
Armed means: on boot our `hub` daemon replaces stock `hal` (`DEMO_MODE=1`), started about 20 s in.

## Arm

1. Build and push (atomic; the old binary survives any failure):
   `tools/deploy_hub.py push --yes --hub <build>/hub --busybox <static-mips-busybox>`
   `push` stages the BusyBox in /tmp and runs its own /data space check (refuses below a 512 KB margin), so `prune` is only needed
   when that check refuses: `tools/deploy_hub.py prune --yes` drops `remote_sim` and `rf_probe`.
2. Token in a file (mode 600) or `HAH_TOKEN`; never on argv.
3. `md5sum <build>/hub`, then:
   `tools/install_boot_hook.py install --yes --token-file F --rf-slot 2 --expect-md5 <md5>`
   Refuses if `/data/ha-harmony/hub` on the device differs from that md5. Keys written to `hub.conf`:
   (the hook launches `hub --token-file /data/ha-harmony/hub.conf --port P ...`, so the token is never on argv.) `token`, `port`, `hub_id`, `rf=spi`, `rf_slot=N`, `config_budget_kib=N` (memory budget of one config load, default 8192, hard-capped by the daemon at 20% of MemFree+Buffers+Cached at its start (below 256 is ignored with a log line), for new pushes only; reported in `hello_ack.limits`), `stop_lua=0|1` (default on: the boot subshell runs `killall lua luaworks luadraws`
   before starting the daemon; `--keep-lua` writes `stop_lua=0`), `reboot_on_early_death=0|1` (default 1;
   `--no-reboot-on-early-death` writes 0). `install` clears the strike counter before arming. If the rcS.local
   replace is not a provable atomic rename it only warns (rcS sources rcS.local under `[ -x ]`, so a missing file
   is a stock boot with SSH up).
4. Marker `/etc/ha-harmony-enable` is created last. Reboot to take effect: `ssh harmony-hub reboot` (the hub has no `sync`;
   the ssh call can hang until the link drops, so run it with `timeout`, backgrounded). Wait with a bounded poll
   (`ssh -o ConnectTimeout=3` every 5 s, give up after about 180 s); never an unbounded loop. Bench: SSH is up about 25-35 s
   after the reboot command, the daemon listens about 45 s in, the health mark lands at about 150 s uptime.
   The daemon is started with `--token-file hub.conf`, so the token is not on its argv. The `rf_spi: link up` line may appear late.

## Status / disarm

- `tools/install_boot_hook.py status`: marker, binary md5, token-redacted conf, strike counter, tails of the
  hook log (`/cache/ha-harmony.log`, truncated each boot) and the daemon log (`/var/volatile/ha-harmony.log`,
  tmpfs, gone on reboot).
- `tools/install_boot_hook.py disarm --yes`: removes only the marker; next boot is pure stock.
- `uninstall --yes [--purge]`: disarm, remove our block and files (`--purge` also conf and log).

**Warning:** stopping the daemon by hand within about 140 s of an armed boot (20 s launch delay + 120 s health
window) makes the hub reboot (`reboot_on_early_death`, default on). Run `disarm --yes` first.
**Watchdog:** the hook refuses to arm (stock boots, no strike) and the installer refuses to install unless
`/etc/nowatchdog` exists. With the stock watchdog enabled, no hal/lua would reboot the hub after each health
mark, clearing the strike every time: an unbounded reboot loop.

## Behaviour to expect

Three unclean boots (daemon not alive 120 s after start) make the hook remove its own marker and boot stock.
A daemon that dies after the 120 s mark is respawned (backoff 5 s, 30 s, 120 s) at most `respawn_max` times per
boot (`hub.conf`, default 3; 0 = old single-shot). Respawns never reboot and never touch the strike counter; the
budget refills after 1 h of continuous uptime; when it is spent the hub stays inert until the next boot. A daemon
that is alive but wedged (heartbeat file `/var/volatile/ha-harmony.hb` older than 60 s) is killed and respawned
on the same budget. `rf_debug=1` in `hub.conf` passes `--rf-debug`. Hook and daemon log lines are prefixed
`[up Ns ...]`.
A daemon that dies BEFORE the mark makes the watcher `reboot` (default; bounded by the strike counter, boot 3 is stock),
but only if this boot's strike was read back from /cache. If /cache cannot hold the counter, the hook leaves stock alone. A WS bind failure or radio-open failure makes
the daemon exit 1 (`hub/main.c`), so "alive at 120 s" implies port bound and radio held.

## Recovery ladder (least invasive first)

1. `ssh harmony-hub rm /etc/ha-harmony-enable`, reboot. Or `disarm --yes`.
2. Factory reset (wipes the whole rw layer, including SSH): re-root per `phase1-hub.md`.
3. UART on J2: the only path if the hook hangs before dropbear (rcS:47). Not expected: the synchronous part
   is file I/O only.

## Factory-reset / Pair button while armed (T1(e))

`hal` is the process that opens `/dev/input/event0`, exposes `libhal_pairing_button_press_notify`, and has a
`/sys/factoryreset` handler (`strings` of stock `/usr/bin/hal`). We found no other reader of that input node
in the extracted rootfs, so the Pair/Reset button is most likely LOST while armed (no `hal`). Not proven: the
decoding path was not disassembled. Treat the recovery ladder as software-only and keep UART access handy.
`luaworks` talks to `hal` over TCP 127.0.0.1 with a retry loop ("HAL connection retry"), so with `hal` absent
it retries and logs; whether that is benign long-term was not determined, hence the `stop_lua` switch.
`/opt/luadraws` does not exist on this hub's rootfs (only `/opt/luaworks`); the `rcS` gate is a no-op.

## Do not image flash while armed

No `device_backup.py` / flash imaging while armed: the daemon holds the shared SPI bus. Disarm, reboot to
stock, confirm `hal` is back, then image.

## On-hub dry run (run 2026-11-18: 15/15 pass)

`tools/install_boot_hook.py selftest --yes` copies the hook and `tools/boot/hook_selftest.sh` into
`/tmp/hah-selftest` (tmpfs) and runs the key cases under the hub's own BusyBox 1.13 `sh` with `killall` and
`reboot` defined as shell functions. It writes only under `/tmp/hah-selftest` and removes it afterwards.
Run it before the first real arm. (BusyBox 1.13 `ls -ln` has extra columns, so the hook caps its log by line count.)

## Elite deploy tooling notes

- Kill switch on the Elite: `tools/deploy_remote.py disable --yes` touches `/data/app/DISABLE`, `enable --yes` removes it;
  both read the file back and print `DISABLE: PRESENT|absent`.
- `tools/deploy_remote.py reboot --yes [--up-timeout SEC]` waits up to 600 s by default (a boot plus the USB link is
  well over 120 s). After each reboot the Elite's USB gadget interface reappears renamed `enx<mac>` (no longer `usb0`)
  and loses the host address: re-address it (`ip link`, then re-add the host address) or the wait will time out.
- `tools/deploy_hub.py run --yes --detach -- <args>` starts the daemon without a tty, logs to `/tmp/ha-harmony-hub.log`,
  confirms the pid is alive and prints it. Both tools need Python >=3.14 (they use 3.14-only syntax, so an older interpreter fails with a SyntaxError): run them with `uv run python tools/<tool>.py ...`.

## Bench gate: respawn (before trusting it)

1. Arm, boot, wait past 150 s (`healthy ... strikes cleared` in `/cache/ha-harmony.log`).
2. `kill` the daemon pid: log shows `respawn 1 of 3 in 5s`, daemon back, HA reconnects, `/cache/ha-harmony-boot.n` absent, no reboot.
3. Kill it 3 more times (each time wait for it to return): 4th death logs `budget exhausted (3 of 3)`, daemon stays down, hub does not reboot.
4. Reboot, then `kill -STOP` the daemon: within ~65 s the log shows `heartbeat stale`, it is killed and respawned.
5. `respawn_max=0` + reboot + kill after 150 s: stays dead (old behaviour).
6. Confirm `ps` shows no token on argv and the daemon log lines carry `[up Ns]`.

## Bench gate: learned slots (opt-in until this passes)

Learning is opt-in: `install_boot_hook.py install --yes --learn-slot ...` (hub) and `deploy_remote.py install-hook --yes --learn-slot` (Elite). Existing installs keep passing `--rf-slot` / `--slot`.

1. Install both with `--learn-slot`; confirm neither daemon argv carries a slot flag.
2. Reboot both. Logs: `slot N learned from link report` (or `from data frame`), then `link up (slot ...)`; `rf_slot` exists next to the hub state file and in `/data/app` on the Elite.
3. Press a key on the Elite: it reaches the hub (HA sees the key event). Push a page from the add-on: it renders on the Elite.
4. Daemon respawn on a live link: `kill` the hub daemon (log `respawn 1 of 3`). No link-up report arrives; the saved slot is used at once and keys/pages still flow (`slot` unchanged in the log, no `send dropped, slot not learned yet`).
5. App restart on a live link: kill `harmony_remote` (start.sh relaunches it): the Elite resumes sending at once and the hub sees its keys.
6. Delete both `rf_slot` files and repeat 4 and 5: the slot is learned from the first data frame instead.
7. Power-cycle the Elite: the slot is re-learned, nothing was sent while unknown.
Only after all pass should the defaults change.

## Bench result: respawn gate (2026-11-19, build 366, passed)

- Kill after health mark: `daemon died after health mark - respawn 1 of 3 in 5s (no reboot)`, daemon back in ~6 s, strike file untouched, no reboot.
- `kill -STOP`: `heartbeat stale for over 60s` logged 65 s after the stop, then the NEXT backoff applies on top (30 s for the 2nd respawn): back after ~100 s total, not 65.
- Backoffs seen: 5 s, 30 s, 120 s; 4th death: `respawn budget exhausted (3 of 3), leaving the hub inert (no reboot)` then `watcher done`; reboot returned to normal (healthy at 153 s).
- The heartbeat file holds the daemon's uptime seconds. Pid is in `/var/volatile/ha-harmony.pid`; `ps` shows no token on argv.
- BusyBox-ash surprises: no `tail`, `head`, `wc`, `stat`, `sleep`-less loops are fine, `grep -E` missing; use `sed -n 'N,$p'` and `cut`. `ps` output is truncated to 40 columns without the pushed /tmp/busybox.
- The hub daemon does not log client hellos; check `/proc/net/tcp` (port 8099 = `:1FA3`, state 01 = established) or a read-only WS client.
- After a hub reboot the RF `link up` line can take 150-200 s; Elite re-announces itself (`remote_info`) once the link is back.

## Bench: measure real flash headroom for the config cache

The hub keeps `2 x hub binary + 256 KiB` free for OTA staging and `state.json`; the cache gets the rest of the
`/data` partition. Measure it instead of trusting a size guess (run on the hub, read-only):

1. `df -k /data` (free KiB) and `ls -l /data/ha-harmony/` (hub, hub.prev, cache.json, state.json sizes). jffs2
   needs ~5% plus a few erase blocks of overhead beyond `df`'s figure; treat `df` free minus ~10% as usable.
2. Connect any client and read `hello_ack.limits.flash_free` (it is `df` free minus the reserve). It should be
   about `df` free minus `2 x hub + 256 KiB`.
3. Push growing configs (`config_chunk`, a large learned-code config) until one is refused with
   `config needs ~N KiB of flash`; confirm `df -k /data` after the refusal is unchanged and that no
   `cache.json.new` remains. Then confirm an OTA stage (`hub.prev` copy) still fits with the largest accepted cache.
4. Record the numbers in this runbook (partition size, free before/after, largest accepted cache).

## Peer ARP keepalive

A peer on the hub's own L2 segment (Home Assistant) may fail to ARP for the hub after a reboot (the hub's Wi-Fi can miss
broadcast ARP); routed clients are unaffected. The hub keeps `peers` (one IPv4 per line, newest first, max 64, private
ranges only) next to `state.json`, adds an address when a peer authenticates (written atomically, only when a new
address appears), and every 30 s (first round at startup) sends one byte of UDP to port 9 of each remembered peer
with no live authenticated connection. The hub's kernel then ARPs for the peer and the peer learns the hub's MAC.
Check on the bench after a hub reboot: `cat /proc/net/arp` on the hub lists the peer within ~30 s and the peer
connects without a manual ping. Deleting `peers` is safe (it is relearned on the next authentication).
