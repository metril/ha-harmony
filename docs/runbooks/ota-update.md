# Runbook: signed hub firmware update

Protocol, file protocol and rationale: `docs/proto/ota-image.md`. Hub only; the remote's RF transport is a later package.
All commands run from the repo root. Device steps are `[WRITE]` (they replace `/data/ha-harmony/hub`): get a go-ahead first.

## Prerequisites

- The hub runs the boot-hook build with this feature (`tools/install_boot_hook.py install`, runbook `phase4-persistence.md`),
  so the hook has the update/rollback logic, and the daemon was started by the hook (default `--ota-dir /data/ha-harmony`).
- A dev-key build needs `ota_allow_dev=1` in `hub.conf` (and `allow_dev_key: true` in the service call); a release build needs neither.
  Updates are refused (`ota_not_ready`) until the boot's health mark (about 2.5 min after boot) and while a previous update is unproven.
- `hub.conf` otherwise unchanged; `respawn_max` and `reboot_on_early_death` at their defaults are fine.
- The running daemon's compiled-in public key matches the key that signs the image. A dev-key build (`-DHP_ALLOW_DEV_OTA_KEY=ON`)
  accepts only `--dev`-signed images; a release build accepts only the release key. Mixing them is `ota_verify: bad signature`.

## Build and sign

```
cmake -S . -B build-hub -DHP_TARGET=hub -DHP_ALLOW_DEV_OTA_KEY=ON ...   # cross build; see the phase 4 plan for the toolchain env
build/ota_sign sign --dev -t hub build-hub/hub hub-<BUILD>.img      # dev key (bench only); the build id is read from the binary
build/ota_sign sign -t hub build-hub/hub hub-<BUILD>.img            # release key (~/.config/ha-harmony/ota_ed25519)
build/ota_sign verify -t hub --dev hub-<BUILD>.img                             # sanity check before shipping
```

The id is the binary's own build id (git commit count, floored by `cmake/build_id_floor.txt`; see `docs/proto/ota-image.md`) and must be newer
than the installed one (`hub: build N` at startup, `fw_build` in hello_ack, `/data/ha-harmony/ota-build`); the hub refuses an equal or lower one
(`ota_downgrade`). Rebuild from a newer commit for every image, including a re-spin of a rolled-back one.

## Install from Home Assistant

1. Put the `.img` under `config/ha_harmony/firmware/` (or another `allowlist_external_dirs` directory).
2. Developer tools > Actions > `ha_harmony.install_firmware`, `path: /config/ha_harmony/firmware/hub-<BUILD>.img`
   (`config_entry` when several hubs are set up).
3. Watch `sensor.<hub>_firmware_update`: `uploading` (attribute `progress`), `applying`, `restarting`, then `installed` once the
   hub is back (about 40-60 s: relaunch by the hook, no reboot). The image is signature-checked by the hub before anything on
   the hub is replaced.
4. If the new build never comes up healthy the hub is back on the previous build by itself and a repair issue
   "Hub firmware update rolled back" appears; the sensor shows `rolled_back` with `rollback_reason`.

On the hub (read-only checks): `/cache/ha-harmony.log` has `daemon exited for a signed update`, `updated build healthy` (or
`updated build died ... restored hub.prev`); `/data/ha-harmony/hub.prev` is the build to fall back to, `ota-pending` is gone
once the build is healthy.

## Bench gate

All four must pass on the hub before this is called done. Record results in the stage plan.

1. **Round trip.** Install build N+1 (a different binary, e.g. rebuilt from a harmless source change). Expect `installed`, the binary md5 changed, `hub.prev`
   present, `ota-pending` absent after the health window, IR/RF still working (send one IR command; a key press reaches HA).
2. **Bad signature rejected.** Flip one byte of the signature (offset 80..143) of a good image and install it. Expect a failed
   service call with `ota_verify` (signature), sensor `failed`, hub never restarted, `md5sum /data/ha-harmony/hub` unchanged,
   no `ota-staged.img`, no `ota-pending`. Repeat with the wrong key (a release-signed image against a dev build).
3. **Truncated image rejected.** `head -c <size-1000> good.img > cut.img`: the integration refuses it client-side (`ota_truncated`).
   To exercise the hub, bypass the client with a scripted `ota_begin` for the full size and send fewer bytes, then `ota_end`:
   `ota_size`, binary unchanged, the upload resumable; an `ota_abort` removes the staging file.
4. **Forced-bad build rolls back.** Sign a binary that exits at once (or `hub --port` bound by another process) as build N+2 and
   install it. Expect: upload and `applying` succeed, the hook relaunches it, it dies inside the health window, `hub.prev` is
   restored and launched, the integration shows the repair issue (build N+2, reason `died_early`), the sensor `rolled_back`,
   the hub serves again on build N+1. Then check it happens once: `hub.prev` is gone, `ota-pending` is gone, and a reboot of the hub
   comes up on build N+1 without another rollback. Finally reinstall a good N+3: the repair issue clears.

Optional power-cut check (needs a go-ahead; the update window is a few seconds): cut power during the upload (staging file is swept
at the next start, binary untouched) and just after `applying` (either the old or the new binary boots; a new one that does not reach
the health mark is rolled back at the next boot).

## Recovery

- Daemon bad and hook rollback did not help: three unclean boots end in stock (the hook disables itself), SSH stays up. Copy a
  known-good binary to `/data/ha-harmony/hub` with `tools/deploy_hub.py push` and re-arm.
- `ota_no_space`: the update needs about twice the image size free on `/data` (the staged copy and the new binary). Free space
  (`tools/deploy_hub.py prune`) and retry; partial files are removed by the hub.
- `ota_busy`: another upload is connected. Wait for it, or reconnect and send `ota_abort` from the owner; if the owner is gone the next
  `ota_begin` takes over.

## Hub filesystem facts (measured 2026-11-19, no OTA run)

- `/data` is a unionfs overlay: `ln` (hard link) works and shares the inode; `mv -f` over an existing file is an atomic rename; `ln` onto an existing name fails (`File exists`), as ota_apply_staged expects. Symlinks work too.
- statvfs works (BusyBox `df`/`stat -f` need the pushed `/tmp/busybox`): 5 MB total, about 2.4 MB free with the 1 MB hub, `remote_sim` and `rf_probe` present. A 2 MB write succeeded. An update needs about 2 MB peak (hub + staged), `hub.prev` is a hard link: tight but fits; run `deploy_hub.py prune` first if it refuses.
