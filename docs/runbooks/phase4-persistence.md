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
   (the hook launches `hub --token-file /data/ha-harmony/hub.conf --port P ...`, so the token is never on argv.) `token`, `port`, `hub_id`, `rf=spi`, `rf_slot=N`, `stop_lua=0|1` (default on: the boot subshell runs `killall lua luaworks luadraws`
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

## Bench gate: learned slots (no `--rf-slot` / `--slot`)

1. Hub: `install_boot_hook.py install --yes --rf ...` (no `--rf-slot`); Elite: `deploy_remote.py install-hook --yes` (no `--slot`). Confirm neither daemon argv carries a slot flag.
2. Reboot both. Hub log: `slot 2 learned from link report` then `link up (slot 2, status 0x87, wpid 4021)`; remote log (`--rf-debug`): `slot 1 learned ...` then `link up (slot 1, status 0x07, wpid 880b)`.
3. Press a key on the Elite: it reaches the hub (HA sees the key event).
4. Push a page from the add-on: it renders on the Elite.
5. Power-cycle the Elite and check the slot is learned again, and no `send dropped, slot not learned yet` repeats once the link is up.
