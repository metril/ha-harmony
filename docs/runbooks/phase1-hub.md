# Runbook: root and back up the hub

Device: Harmony Hub, USB `046d:c129` (`/dev/hidraw3`), LAN `192.0.2.11`, stock FW 4.15.600.
External root tool: `/home/user/projects/upstream-ref/harmony-ng` (MIT, run as a tool).
Our key: `~/.ssh/harmony_owner_ed25519`.

Run the steps in order. Each is marked `[RO]` (read-only on the hub) or `[WRITE]`.
**Stop at the first unexpected result** and read "Recovery" below rather than improvising.

## Read this first: what cannot be backed up

Full partition images need root, and root needs a write. So **there is no way to image the hub in its
pristine stock state** — our images will always show a hub that already has Dropbear staged. The
pre-root artifacts we do have:

- `backups/phase1/hub/pre-root-config/` — the complete stock config pulled read-only over the hub's
  local WebSocket API (6 devices, 3 activities, 133 commands). Taken 2026-11-16, before any write.
- The external tool's own tar of `/cache` + `/data`, taken immediately on gaining root, before
  anything else is installed.

Exactly what the tool writes (all recoverable by the hardware factory reset, which wipes
`/data` + `/cache`):

| Path | Change |
|---|---|
| `/data/dropbearmulti` | MIPS Dropbear binary, extracted on-device from the stock `/usr/sbin/dropbearmulti` |
| `/etc/dropbear/authorized_keys`, `/root/.ssh/authorized_keys`, `/home/root/.ssh/authorized_keys` | our public key, mode 600 |
| `/etc/dropbear/dropbear_rsa_host_key` | generated if absent |
| `/usr/sbin/dropbear` | **replaced** with a wrapper execing `/data/dropbearmulti dropbear -s -g` (key-only, password login refused) |
| `/usr/sbin/dropbearkey`, `dbclient`, `scp` | symlinks to `/data/dropbearmulti` |
| `/etc/tdeenable` | armed, so Dropbear starts on every boot |
| `/etc/passwd` | root password emptied |
| `/etc/nowatchdog` | stock watchdog disabled |

Note the `/usr/sbin/*` entries: if `/usr` sits on the read-only squashfs those writes simply fail and
Dropbear still starts via the TDE gate; if `/usr` is on a writable layer they are wiped by the
factory reset. Either way nothing touches `mtd0` (U-Boot), `mtd1`/`mtd2` (kernels) or the squashfs
partition itself — no partition is ever flashed.

## Before you start

- [ ] UART adapter plugged in and **confirmed 3.3 V TTL** (never 5 V on J2: pin 2 RX, pin 4 TX, pin 8
      GND, 115200 8N1). Nothing is on `/dev/ttyUSB*` until it is plugged in. Install a terminal:
      `sudo apt install picocom`.
- [ ] Hub blocked from WAN (MAC filter or offline SSID) so it cannot pull a stock update mid-run.
      It is already at the latest version, so this is belt-and-braces.
- [ ] Do not open MyHarmony or the Harmony phone app for the rest of Phase 1.
- [ ] `ls -l /dev/hidraw3` shows `plugdev` (if not: `sudo tools/host_setup.sh`).
- [ ] Hub reachable: `ping -c2 192.0.2.11`, port 8088 open, port 22 **closed**.

## Step 1 `[RO]` — confirm first contact

```sh
cd /home/user/projects/upstream-ref/harmony-ng
uv run --no-project --python 3.13 python tools/harmony_firmware.py check
```

Expect skin 97 → 4.15.600 (hub) and skin 111 → 4.15.330 (remote) — i.e. the latest firmware is what
we already run, so `--skip-fw-check` below skips nothing we want.

## Step 2 `[WRITE]` — root the hub

Needs an explicit go-ahead. Keep the UART adapter within reach.

```sh
cd /home/user/projects/upstream-ref/harmony-ng
uv run --no-project --python 3.13 python tools/root_hub_usb.py \
    --hub-ip 192.0.2.11 \
    --skip-fw-check \
    --pubkey ~/.ssh/harmony_owner_ed25519.pub
```

- `--hub-ip` is passed so the tool does not subnet-scan (the hub is on another subnet, already routed).
- **Do not** pass `--no-backup` — we want its stock tar.
- **Do not** pass `--wifi-ssid`/`--wifi-pass`: reachability already works, so there is no reason to
  rewrite the hub's Wi-Fi config.
- **Do not** run its `deploy_hub.py` at any point in Phase 1. Rooting only.

The hub reboots at the end. The tool then verifies SSH and streams a tar of `/cache` + `/data` into
`backups/stock_hub_backup_<ts>.tar`, copied to `stock_hub_backup_latest.tar` and (once only)
`golden_hub_backup.tar`. It stages a BusyBox with `tar` into `/tmp` to do this, because stock BusyBox
lacks it.

## Step 3 `[RO]` — gate: root and stock tar

```sh
ssh -i ~/.ssh/harmony_owner_ed25519 root@192.0.2.11 "uname -a; id; cat /proc/mtd; mount"
ls -l /home/user/projects/upstream-ref/harmony-ng/backups/
```

Both must succeed before going further. If SSH works but the tar is missing (the tool has a short
window for it), take it immediately — before anything else touches the hub:

```sh
uv run --no-project --python 3.13 python tools/backup_hub.py 192.0.2.11
# add --reset-host-key if ssh complains the host key changed
```

Record here: the real `/proc/mtd` output, `uname -a`, and the tar's size and sha256.

## Step 4 `[RO]` — full partition images

```sh
cd /home/user/projects/ha-harmony
TS=$(date -u +%Y%m%dT%H%M%SZ)
uv run tools/device_backup.py --target hub --host 192.0.2.11 \
    --key ~/.ssh/harmony_owner_ed25519 \
    --out-dir backups/phase1/hub/$TS --dry-run
```

Check the dry-run plan against the real `/proc/mtd` from step 3 — partition count, names, sizes and
the chosen read method — then drop `--dry-run` and run it for real. Then:

```sh
uv run tools/device_backup.py --verify backups/phase1/hub/$TS
```

Every partition must be `ok` with the device-side and host-side hashes matching. Statuses that mean
stop and investigate rather than proceed:

- `hash_failed` — the image may well be intact, but the device-side hash pass failed. Re-run that
  partition with `--only`; do not accept the image on a failed verification.
- `corrupt` — hashes genuinely disagree; the file is kept as `*.img.CORRUPT`.
- `error` with "declared size 0" — a partition reported zero size; investigate, never treat as backed up.

## Step 5 `[RO]` — second copy

Copy the whole `backups/phase1/hub/$TS/` directory **and** the external tool's tars somewhere that is
not this disk, then re-hash after transfer:

```sh
sha256sum backups/phase1/hub/$TS/*.img | tee backups/phase1/hub/$TS/SHA256SUMS
# after copying, on the destination: sha256sum -c SHA256SUMS
```

## Step 6 `[RO]` — restore rehearsal

Learn the restore path before needing it. This command prompts for a typed `RESTORE`:
**abort there — answer anything else.**

```sh
cd /home/user/projects/upstream-ref/harmony-ng
uv run --no-project --python 3.13 python tools/restore_stock.py \
    --skip-usb-root --hub-ip 192.0.2.11 --backup backups/golden_hub_backup.tar
```

Caveats to remember when it is for real:

- A fresh root run overwrites `stock_hub_backup_latest.tar` with a backup of the *current* hub.
  **Always restore from the dated or `golden_` tar.**
- A factory-reset hub has no BusyBox with `tar`, so a restore has to stage one again.

## Recovery

In order, least invasive first:

1. **Software restore** — `restore_stock.py` as above, typing `RESTORE` for real.
2. **Hardware factory reset** — unplug, hold Pair/Reset, plug USB in while holding, keep holding
   15–20 s until the LED flashes red rapidly, release. Wipes `/data` + `/cache` (i.e. everything the
   root tool wrote); ~30–45 s to stock. This is the main safety net and needs no disassembly.
3. **UART** — J2 pin 2 RX, pin 4 TX, pin 8 GND, 3.3 V TTL, 115200 8N1. Interrupt U-Boot, then
   `setenv bootargs console=ttyS0,115200 init=/bin/sh; bootm 0x9f010000`. For a hub that boot-loops
   past the reset. A boot-failure counter also drops the hub into an emergency recovery mode
   (Wi-Fi + SSH) by itself after repeated failed boots.

## Log

| When (UTC) | Step | Result |
|---|---|---|
| 2026-11-16 | pre-root config fetch `[RO]` | 6 devices, 3 activities, 133 commands, no raw IR present |
| 2026-11-16 | firmware check `[RO]` | latest = installed (hub 4.15.600, remote 4.15.330) |
