# Phase 4 runbook: hub boot persistence

Plan and rationale: `docs/plans/2026-11-17-phase4-persistence.md`. All commands run from the repo root.
Armed means: on boot our `hub` daemon replaces stock `hal` (`DEMO_MODE=1`), started about 20 s in.

## Arm

1. Build and push (atomic; the old binary survives any failure):
   `tools/deploy_hub.py push --yes --hub <build>/hub --busybox <static-mips-busybox>`
   then `tools/deploy_hub.py prune --yes` to drop `remote_sim` and `rf_probe` if space is tight.
2. Token in a file (mode 600) or `HAH_TOKEN`; never on argv.
3. `md5sum <build>/hub`, then:
   `tools/install_boot_hook.py install --yes --token-file F --rf-slot 2 --expect-md5 <md5>`
   Refuses if `/data/ha-harmony/hub` on the device differs from that md5. Keys written to `hub.conf`:
   `token`, `port`, `hub_id`, `rf=spi`, `rf_slot=N`, `stop_lua=0|1` (default on: the boot subshell runs `killall lua luaworks luadraws`
   before starting the daemon; `--keep-lua` writes `stop_lua=0`), `reboot_on_early_death=0|1` (default 1;
   `--no-reboot-on-early-death` writes 0). `install` clears the strike counter before arming. If the rcS.local
   replace is not a provable atomic rename it restores and refuses unless `--allow-nonatomic-rcs-local`.
4. Marker `/etc/ha-harmony-enable` is created last. Reboot to take effect.

## Status / disarm

- `tools/install_boot_hook.py status`: marker, binary md5, token-redacted conf, strike counter, tails of the
  hook log (`/cache/ha-harmony.log`, truncated each boot) and the daemon log (`/var/volatile/ha-harmony.log`,
  tmpfs, gone on reboot).
- `tools/install_boot_hook.py disarm --yes`: removes only the marker; next boot is pure stock.
- `uninstall --yes [--purge]`: disarm, remove our block and files (`--purge` also conf and log).

## Behaviour to expect

Three unclean boots (daemon not alive 120 s after start) make the hook remove its own marker and boot stock.
Single-shot: a daemon that dies after the 120 s mark stays dead until the next boot (no reboot, no respawn).
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
