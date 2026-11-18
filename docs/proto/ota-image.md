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

## Hub apply (library only)

`ota_apply_staged(staged, target, prev, min_build, pubkey, &build)` verifies, writes `<target>.new`, hard-links the
current binary to `hub.prev`, then atomically renames over the target. Verify and copy share one open file and the copied bytes are re-hashed against the signed digest before the rename (no TOCTOU); `ota_rollback()` renames `hub.prev` back.
Not wired into the boot hook.

## Remaining

- Transport: hub via WebSocket/HTTP upload to a staging file; remote via RF fragments into the remote's `/data`.
- Remote apply (A/B) using the same verify.
- Boot-hook integration: apply on boot, boot-loop counter, automatic rollback to `hub.prev`.
- HA service to push an update, `proto/schema.yaml` messages, release-key generation and signing procedure.
- Hardware gate: OTA round-trip, bad signature rejected on device.
