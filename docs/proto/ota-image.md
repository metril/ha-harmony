# Signed OTA image (stage G, first slice)

One container for hub and remote firmware. Code: `common/ota/` (shared), `tools/ota_sign.c` (host),
vendored TweetNaCl in `vendor/tweetnacl/`.

## Format (little-endian)

| off | size | field |
|----:|-----:|-------|
| 0 | 4 | magic `HOTA` |
| 4 | 1 | format version (1) |
| 5 | 1 | target: 1 hub, 2 remote |
| 6 | 2 | flags (must be 0) |
| 8 | 4 | build id (monotonic) |
| 12 | 4 | payload length (max 32 MiB) |
| 16 | 64 | SHA-512 of the payload |
| 80 | 64 | Ed25519 signature over bytes 0..79 |
| 144 | n | payload |

File size must be exactly `144 + length`. The signature covers the digest, so verifying a staged file is one
streaming pass (4 KiB buffer). Checks, in order: header values, length cap, signature, target, `build >= min_build`
(downgrade), file size, digest. Any failure leaves the installed firmware untouched.

## Keys

The public key is compiled in from `HP_OTA_PUBKEY_HEADER` (default: `common/ota/ota_pubkey_dev.h`, the DEV/TEST key
whose seed is bytes 1..32). A build with the dev key warns loudly at configure time and the hub/remote print "dev OTA key built in" at startup.
Cross builds, `HP_RELEASE=ON` and any non-Debug build type refuse it (CMake error, plus `#error`) unless
`-DHP_ALLOW_DEV_OTA_KEY=ON` (CI and the documented dev cross-build pass it). `ota_pubkey_is_dev()` reports it at runtime.

Release key: `ota_sign keygen` writes the seed to `~/.config/ha-harmony/ota_ed25519` (mode 0600, outside the repo,
never committed; CI never signs; releases are signed by hand). `ota_sign pubheader > key.h`, then build with
`-DHP_OTA_PUBKEY_HEADER=$PWD/key.h`.

```
ota_sign keygen | pubkey | pubheader [--dev]
ota_sign sign -t hub|remote -b BUILD payload out.img
ota_sign verify [-t hub|remote] [-m MINBUILD] [-p PUBHEX | -k KEY] out.img
```

## Hub update over ws

Messages (`proto/schema.yaml`; every reply carries the request id):

| id | name | fields | notes |
|---:|------|--------|-------|
| 20 | `ota_begin` | `target` (1), `size`, `build`, `sha` | `size` = whole image bytes, `sha` = hex SHA-512 of the whole file (transfer check); refused: target not hub, size outside 144..144+32 MiB, build not newer than the running one (floor = max(`ota-build`, compiled-in `HP_BUILD_ID`); an equal build is refused too: `ota_downgrade`), too little space (`statvfs`, needs 2x size + 64 KiB when it reports) |
| 21 | `ota_chunk` | `offset`, `data` (base64) | at most 4096 bytes decoded; must start at the hub's offset (a retransmit of held bytes is accepted, a gap is `ota_offset`) |
| 22 | `ota_end` | | hub checks size, whole-file digest, declared == signed build, then `ota_apply_staged` |
| 23 | `ota_abort` | | drops the upload (owner only) |
| 24 | `ota_status` | `state`, `offset`, `size`, `build?`, `reason?` | reply to begin/chunk/end/abort: `receiving`, `applied`, `aborted`; pushed after every hello as `rolled_back` while `ota-rollback` exists |

Errors are `error` with code `ota_unsupported` (no `--ota-dir`), `ota_busy`, `ota_state`, `ota_offset`, `ota_size`,
`ota_target`, `ota_chunk`, `ota_no_space`, `ota_io`, `ota_sha`, `ota_verify` (message names the signature/digest/format
failure), `ota_downgrade`, `ota_not_ready` (the hook has not written its health mark this boot, or `ota-pending` still exists), `ota_dev_key`, `ota_bad_request`.

One upload at a time. A second `ota_begin` from another connection while the first is connected is `ota_busy`; when the
owner's connection drops the upload stays and any `ota_begin` with the same size/build/sha resumes it (the reply's
`offset` says where); a different `ota_begin` replaces a suspended one. Staging is `ota-staged.img` next to the binary
(on `/data`); a write error (ENOSPC included) removes the partial file and fails the upload. The staging file does
not survive a daemon restart.

Ids 30..34 (`ota_offer`, `ota_frag`, `ota_frag_ack`, `ota_commit`, `ota_result`; transport rf) are reserved for the
remote's RF transport and are not implemented. 25..29 are spare.

## Apply, restart, rollback

Files in the daemon's data dir (`/data/ha-harmony`; `--ota-dir`, default on a real hub, off under `--sim`):

| file | writer | meaning |
|------|--------|---------|
| `hub` | apply | the binary the boot hook runs |
| `hub.prev` | apply | hard link to the build before the last update |
| `ota-pending` | daemon, after `hub.prev` is in place and immediately before the rename; hook at the health mark | `build=N`, `prev=M`: a fresh build that has not proven itself. A cut earlier leaves `hub.prev` hard-linked to `hub`, so the hook's rename is a no-op and an older `hub.prev` can never be installed |
| `ota-restart` | daemon after the swap | asks the hook for an immediate relaunch (it cannot see the exit status) |
| `ota-build` | daemon (with the marker), hook on rollback (`prev=M`) | build id of the installed binary: the downgrade floor |
| `ha-harmony.healthy` (tmpfs) | hook | written at the health mark; the daemon (`--health-file`) refuses `ota_begin` until it exists. Removed while an updated build is on probation |
| `ota-rollback` | hook | `build=N`, `reason=boot_unclean\|died_early`, `at=...`: what was rolled back and why |

`ota_apply_staged` verifies, copies the payload to `hub.new` re-hashing it against the signed digest, hard-links the
running binary to `hub.prev` and renames `hub.new` over `hub` (atomic: a power cut leaves the old or the new binary,
never a mix; leftovers are swept at the next start). Then the daemon replies `applied`, leaves `ota-restart`, flushes
and exits with status 42. It never execs or replaces itself.

Boot hook (`tools/boot/rcS.harmony`):

- `ota-restart` present when the daemon dies: relaunch at once (no respawn budget, no strike) and give the new build the
  normal health window. Alive at the end: `ota-pending` is removed, `hub.prev` stays. Dead inside it: `hub.prev` is
  renamed over `hub`, `ota-rollback` written, the previous build launched.
- `ota-pending` present at boot (the new build rebooted, crashed under `reboot_on_early_death`, or lost power before the
  health mark): same rollback, strike counter cleared (the strikes were the new build's).
- The rename consumes `hub.prev` and the marker is removed either way, so a rollback happens once per update. If the
  restored build is bad too, the ordinary strikes apply and boot 3 is stock, as before.
- A marker with no `hub.prev` (power cut before the swap) or a `hub.prev` identical to `hub` only clears the marker.

The daemon reports `ota-rollback` as `ota_status rolled_back` after every hello until the next successful update
removes it. The integration turns that into a repair issue.

## Dev key

The compiled-in dev key's seed is public. A build carrying it accepts updates only when started with `--ota-allow-dev` (hook conf
`ota_allow_dev=1`, default 0); otherwise `ota_begin` is `ota_dev_key`. `hello_ack` carries `fw_build` and `ota_key` (`dev` or
`release`) whenever updates are enabled. The integration shows both on the Firmware update sensor, raises a repair issue for a dev-key hub and
refuses `install_firmware` on it unless the call sets `allow_dev_key: true`. Set the running build id with `-DHP_BUILD_ID=N`.

## Home Assistant

Service `ha_harmony.install_firmware {path, config_entry?}`. A service rather than an `update` entity because there is no
release feed to compare versions against: an install is a deliberate act on a local, signed file. The path must be under
`config/ha_harmony/firmware/` or an `allowlist_external_dirs` entry (symlinks are resolved first). The integration checks
the `HOTA` magic, format, target (hub) and exact file size for a friendly error and never sends anything else; the hub's
verification is the authority. Progress: `sensor.<hub>_firmware_update` (`idle`, `uploading`, `applying`, `restarting`,
`installed`, `failed`, `rolled_back`; attributes `progress`, `build`, `error`, `rollback_reason`). A dropped connection
mid-upload is resumed from the hub's offset (3 attempts). `ota_end` is sent once and never retried: after it only the hub's reconnect
reporting the new `fw_build` (or a rollback of that build) decides the outcome. A rollback becomes a warning repair issue
`ota_rolled_back_<hub>_<build>`, cleared by the next successful install. Procedure and bench gate:
`docs/runbooks/ota-update.md`.

## Remaining

- Remote: RF fragment transport (ids 30..34 reserved) and A/B apply on the remote with the same verify.
- Release-key generation and signing procedure for shipping builds, and a hub-side health mark richer than "port bound
  and still alive for the window".
