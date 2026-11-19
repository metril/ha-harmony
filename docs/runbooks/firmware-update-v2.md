# Runbook: firmware update to page format v2 (hub + remote together)

Hub and remote must run the same page format (v2); update both in one window, then reboot hub first, remote second.
All commands run from the repo root. `uvp` = `uv run --python 3.14 python` (the tools need Python 3.14).
Both devices are already armed (hub marker, remote hook); this changes binaries only. Fallbacks stay intact:
hub strike counter / `install_boot_hook.py disarm`; remote `/data/app/DISABLE`.

## 1. Pre-checks (read-only)

1. Hub: `ssh harmony-hub cat /proc/uptime` (there is no `uptime`). Do not stop the daemon within ~140 s of boot.
2. Elite on USB: host needs `192.168.2.2/24` on the gadget interface (`ip -br a`; if it is `enx<mac>` without an address,
   `nmcli dev modify <if> ipv4.method manual ipv4.addresses 192.168.2.2/24`, no sudo).
3. `uvp tools/install_boot_hook.py status` (armed, strike clear, no `DISABLE`), `uvp tools/deploy_remote.py status`
   (hook installed, counter 0, powersource usb, battery not low).
4. Backups exist: `backups/phase1/{hub,remote}` plus the previous binaries. Copy the currently deployed binaries (new files,
   never overwrite): `ssh harmony-hub 'cat /data/ha-harmony/hub' > backups/<dir>/hub.<md5>`,
   `ssh -i ~/.ssh/harmony_owner_ed25519 root@192.168.2.1 'cat /data/app/harmony_remote' > backups/<dir>/harmony_remote.<md5>`
   (no sftp on either device, so no scp).

## 2. Build and test

1. Hub: `HP_TOOLCHAIN_ROOT=$PWD/vendor/toolchains/mips_24kc cmake --build build-cross -j8 --target hub`
2. Remote: `HP_TOOLCHAIN_ROOT=$PWD/vendor/toolchains/armv5-eabi cmake --build build-remote -j8 --target harmony_remote`
3. `cmake --build build-host -j8 && ctest --test-dir build-host -j8`; `uv run pytest tests/py -n auto -q`.
4. `python3 tools/check_hub_size.py build-cross/hub` (the cross build defaults to MinSizeRel: -Os, gc-sections, a stripped `hub`, with the
   symbols in `hub.debug`; it fails over 600 KiB, and prints the OTA need of 2x image + slack against the ~2.4 MB /data). Push and sign
   only the stripped `hub`; `deploy_hub.py push` refuses an unstripped one.
5. `md5sum build-cross/hub build-remote/harmony_remote` and note them.

## 3. Push (files only; running processes unchanged)

1. Hub: `uvp tools/deploy_hub.py push --yes --hub build-cross/hub --busybox ~/projects/upstream-ref/harmony-ng/payload/bin/busybox`
   (atomic, md5-verified, old binary survives failure; run `prune --yes` only if the space check refuses).
2. Remote: `uvp tools/deploy_remote.py push --yes build-remote/harmony_remote`.
3. Hook md5 guard: `uvp tools/install_boot_hook.py status` shows the hub md5 = the new build.

## 4. Reboot order and cold-boot check

1. Hub first: `timeout 10 ssh harmony-hub reboot` (background). SSH returns in ~30 s, daemon listens ~45 s, health mark ~150 s uptime.
2. Remote second, after the hub listens: `uvp tools/deploy_remote.py reboot --yes` (run it in the background: it needs >120 s).
   Every remote reboot re-enumerates the USB gadget with a NEW `enx<mac>` name and no address, so the tool cannot reach it; re-run the
   `nmcli dev modify` from pre-check 2 on the new name (seen 2026-11-18: this was "not authorized" while NM sat in
   "connecting (getting IP configuration)"; a host-side `sudo ip addr add 192.168.2.2/24 dev <new enx>` by the owner is the fallback).
3. Confirm each came up by itself on the new md5 (hub `status`, remote `status`, process argv and md5).

## 4a. Verification

- Hub daemon log (`install_boot_hook.py status`): `rf_spi: link up (slot 2 ...)`, a `remote_info` line with gen/pages, a key-press line.
- Remote log (`ssh ... cat /var/volatile/ha-harmony-app.log`): boot, RF link, page received/rendered.
- Press a key on the remote and see it reach the hub log; push a page (HA/add-on or `deploy_hub.py press`/remote_sim) and see it render.
- Hook log shows `healthy after 120s - strikes cleared`; `deploy_remote.py status` counter back to 0.

## 4b. What reality showed (2026-11-18)

- The hub logs almost nothing (5 lines). Verify RF from a read-only WS client instead: hello with the token from `hub.conf`
  (read it over ssh, never print it); on connect the hub sends `remote_info` (`page_fmt`, `caps`, `gen`, `pages`) and later
  `remote_attached`, `battery` and `key_event` broadcasts. `page_fmt 2` proves the new remote build is announcing over RF.
- Remote log (`/var/volatile/ha-harmony-app.log`) is also nearly silent by default; check the running build with
  `md5sum /proc/<pid>/exe`, and the screen with `cat /dev/fb0` (header `240,960`/16 bpp text first, then 3 RGB565 buffers of 240x320).
- With HA still on the old integration (0.2.x, YAML) the remote gets v1 pages: it shows them via the v1 fallback and reports
  `gen 0 pages 0`. Full v2 verification needs the 0.3.0 integration + add-on config pushed from HA.
- The remote's gadget interface changes its `enx` name on every remote reboot and loses its address; it needs
  `sudo ip addr add 192.168.2.2/24 dev <enx>` from the owner (nmcli may refuse).

## 5. Rollback

- Hub: `install_boot_hook.py disarm --yes` + reboot returns stock; or put back the saved `hub.<md5>` via `deploy_hub.py push --hub <saved>`.
  Three unclean boots self-disarm.
- Remote: `ssh ... touch /data/app/DISABLE` stops our app; or `deploy_remote.py push --yes <saved harmony_remote>`; last resort Menu+Off.
- Never flash partitions. HA is untouched by this procedure.

## 4c. Restarting the remote app without a reboot (2026-11-19)

`kill` the `start.sh` and `harmony_remote` pids, then `ssh root@... '(/data/app/start.sh </dev/null >/dev/null 2>&1 &)'`. A plain `nohup ... &` dies with the ssh session. The new start.sh prefixes log lines with `[up Ns]`. A freshly started app announces `gen 0 pages 0` until Home Assistant pushes pages again.
