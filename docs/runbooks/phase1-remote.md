# Runbook: root and back up the Elite remote

Device: Harmony Elite, USB `046d:c12b` (bottom micro-USB port, **not** the cradle), stock FW 4.15.330.
After rooting it is reachable over USB Ethernet at `192.168.2.1` (host side `192.168.2.2/24`).
External root tool: `~/projects/upstream-ref/harmony-ng` (MIT, run as a tool).
Our key: `~/.ssh/harmony_owner_ed25519`.

Run the steps in order. Each is marked `[RO]` (read-only on the remote) or `[WRITE]`.
**Stop at the first unexpected result** and read "Recovery" below rather than improvising.

## Read this first

- **The screen goes blank and stays blank.** Rooting stops the stock UI (`remote_ui`) and never starts
  it again. A blank screen after Step 2 is the expected outcome, not a brick — SSH is the interface
  until our own remote app is deployed in Phase 4 Stage D. The remote is out of service for the
  living room from this point until then.
- **Step 1 permanently disables the remote's USB HID mode.** That is how the rooted boot path is
  armed; it is undone only by the hardware factory reset.
- **The only recovery path is Menu+Off.** Unlike the hub, the Elite has no documented UART header and
  no emergency network mode. Everything this runbook writes lands in the writable layer, which that
  reset wipes — so keep every write inside `/data`, `/opt` and `/etc`, and never flash a partition.
- Like the hub, **the remote cannot be imaged in its pristine stock state**: imaging needs root and
  root needs a write. The images will always show a remote with Dropbear already staged.

What the root writes (all wiped by Menu+Off):

| Path | Change |
|---|---|
| `/data/dropbearmulti` | ARM Dropbear binary |
| `/sys/dropbear` | self-extracting payload, staged over USB HID (transient) |
| `/etc/tdeenable` | armed, so the rooted multi-user path runs on every boot |
| `/etc/nowatchdog` | stock watchdog disabled |
| `/etc/init.d/rcS.local` | replaced with the USB-Ethernet + SSH boot script |
| `/data/remote_ui` | removed (this is why the screen stays blank) |

## Before you start

- [ ] **Hub completely done and gated first.** The remote has no UART fallback; do not start here
      until the hub's Phase 1 gate has passed.
- [ ] Remote at FW 4.15.330 and charged. Do not open MyHarmony or the Harmony phone app.
- [ ] Connect the **bottom micro-USB port** on the remote itself, not the charging cradle.
- [ ] `lsusb | grep 046d:c12b` shows the remote; `ls -l /dev/hidraw*` shows `plugdev`
      (if not: `sudo tools/host_setup.sh`).
- [ ] Build the static ARM BusyBox the upstream tool needs for its own tar backup — without it that
      backup silently does not happen, because the stock BusyBox has no `tar`:

      ```sh
      cd ~/projects/upstream-ref/harmony-ng
      PATH=~/projects/ha-harmony/vendor/toolchains/armv5-eabi/bin:$PATH \
        ./build/build_busybox.sh arm
      file remote/bin/busybox   # ELF 32-bit LSB, ARM EABI5, statically linked
      ```

      Our vendored Bootlin toolchain is used via `PATH`; nothing is added to the upstream clone's
      own `build/toolchains/`.

## Step 1 `[WRITE — go-ahead needed]` — arm the rooted boot path

```sh
cd ~/projects/upstream-ref/harmony-ng
uv run --python 3.14 python remote/setup_remote.py --step 1 --skip-fw-check \
  --key ~/.ssh/harmony_owner_ed25519 < /dev/null
```

`--skip-fw-check` is mandatory: the check can offer to install a stock update, which must not happen
before the backup. `< /dev/null` refuses that prompt by construction.

Expect: the Dropbear payload staged over USB HID, `/etc/tdeenable` created, a reboot, then the
remote re-enumerating as a USB Ethernet (RNDIS) gadget.

The tool's own wait for SSH **times out, and that is harmless** — it waits before the host has an
address on the new interface. Give the host one:

```sh
sudo ip addr add 192.168.2.2/24 dev usb0     # no default route on this interface
sudo ip link set usb0 up
```

## Step 2 `[RO]` — gate: SSH and the real partition layout

```sh
ssh -i ~/.ssh/harmony_owner_ed25519 root@192.168.2.1 "uname -a; cat /proc/mtd; mount"
```

Gate: this returns `Linux juniper 2.6.37-ge384483 ... armv5tejl`, 11 mtd partitions, and a unionfs
root. If SSH does not answer, check `ip -br link` for `usb0` before touching the remote again.

### Corrections to the design spec, verified on the device (2026-11-17)

- **11 mtd partitions, not the 3-way "squashfs root + /data + /opt" the spec assumed:**
  mtd0 `u-boot env` 128K, mtd1 `ubl` 640K, mtd2 `u-boot` 1280K, mtd3 `ubi` 126M,
  mtd4 `kernel` / mtd5 `kernel2` 2108K each, mtd6 `root` 30M, mtd7 `data` 50M,
  mtd8 `cache` 34M, mtd9 `mfg` 248K, mtd10 `log` 1116K.
- **mtd3 `ubi` is the superset.** mtd4–mtd10 have erasesize `0x1f000`, the UBI LEB size — they are
  gluebi volumes living inside mtd3, not independent flash regions. Imaging both is redundant but
  complete, and the backup does so deliberately.
- **Root is unionfs, as on the hub:** `/dev/mtdblock6` squashfs at `/mnt/root` (ro) under
  `ubi0:data` at `/mnt/data` (rw, ubifs), with `ubi0:cache` on `/cache`. Writes to apparently-rootfs
  paths land in mtd7 and are wiped by Menu+Off.
- **The stock BusyBox is even thinner than the hub's**: no `dd`, `sha256sum`, `tar`, `head`, `find`,
  `uptime`. `cat`, `md5sum`, `awk`, `which`, `sed`, `grep`, `cp`, `ps`, `netstat` exist.
- **Only `/dev/mtdN` and `/dev/mtdblockN` exist** — there are no `/dev/mtdrN` or `/dev/mtdNro` read
  nodes, unlike the hub.
- The accelerometer is a **LIS3DH** (`irq/108-LIS3DH` in `ps`), and SPI appears as `spi_davinci.1`.

## Step 3 — `--step 2` is unnecessary here, and will fail

`setup_remote.py --step 2` installs persistent SSH *and* upstream's `remote_ui`. Do not run it:

- It exits 1 immediately with `Cannot reach 192.168.2.1:8088`. Its command channel is the stock
  HTTP service on port 8088, which no longer runs once the remote is in rooted mode — SSH has
  replaced it.
- **Step 1 already leaves persistent SSH in place.** Verified on the device: `/data/dropbearmulti`
  listening on `0.0.0.0:22`, plus `/etc/nowatchdog`, `/etc/tdeenable` and the replaced
  `/etc/init.d/rcS.local`. Nothing in step 2 is needed to keep SSH across a reboot.
- The one thing step 2 adds on top is upstream's `remote_ui`, which we do not want: our own LVGL app
  replaces it in Stage D. This also resolves Phase 1 open question 3 — nothing of upstream's
  userspace is installed, so the `/data` image stays free of it.

Consequence: **upstream's tar backup never runs**, so the full partition images in Step 4 are the
only backup of this device. The ARM BusyBox built in "Before you start" is therefore not strictly
required on this path, but build it anyway — it is the fallback if step 2 ever has to be run.

## Step 4 `[RO]` — full partition images

```sh
cd ~/projects/ha-harmony
TS=$(date -u +%Y%m%dT%H%M%SZ)
uv run python tools/device_backup.py --target remote --host 192.168.2.1 --user root \
  --key ~/.ssh/harmony_owner_ed25519 --out-dir backups/phase1/remote/$TS --dry-run
uv run python tools/device_backup.py --target remote --host 192.168.2.1 --user root \
  --key ~/.ssh/harmony_owner_ed25519 --out-dir backups/phase1/remote/$TS
```

**The tool now skips mounted gluebi volumes automatically.** It reads `/proc/mounts` and, for every
writable `ubi<N>:<name>` ubifs mount, never touches the mtd partition of that name (`data` and
`cache` here); it records them in the manifest as `skipped` with the reason `mounted gluebi volume
(ubi0:data, rw) — contained in mtd3 'ubi'`. Read-only mounts such as the squashfs `root`
(`/dev/mtdblock6`) are still imaged. `--verify` prints the skips as `SKIPPED` and treats them as OK,
so **a re-run should produce a passing `--verify`**. The run history below (20261117T050746Z) predates
this and is kept as it happened.

Two warnings are expected and correct on this device:

- `dd missing on device; reading partitions with cat` — the supported fallback path.
- `sha256sum missing on device; falling back to WEAKER md5sum` — the **device-side** hash is md5;
  the local hash over the received bytes is still sha256. The pair still catches transport
  corruption, which is what the cross-check is for. The alternative — staging our BusyBox to get
  `sha256sum` — was rejected so the backup stays strictly read-only, with nothing of ours on the
  device.

~260 MB over USB Ethernet with `cat`; expect this to run for a while. Then verify offline:

```sh
uv run python tools/device_backup.py --verify backups/phase1/remote/$TS
```

### History: `--verify` FAILED on run 20261117T050746Z, for four partitions

Run 20261117T050746Z (before the tool skipped mounted gluebi volumes), the Phase 1 remote backup of record:

- `ubl` (mtd1) and `u-boot` (mtd2) fail their `mtdblock` read with `cat: read error: Input/output
  error`. The **raw `mtd` read of both succeeds and hash-verifies** — raw is the authoritative read
  for NAND, so only the redundant variant is missing.
- `data` (mtd7) and `cache` (mtd8) **stall** (`no data for 60s`). These are *mounted* gluebi volumes:
  `/mnt/data` is the live rw layer under the unionfs root. Reading a mounted gluebi volume through
  its mtd node is the known-bad case. **Do not retry it** — a hung read risks wedging a device whose
  only recovery is the factory reset that destroys the root, and the content is already captured.
- Both are fully contained in the verified `03_ubi` image, because mtd4–mtd10 are gluebi volumes
  inside mtd3 (erasesize `0x1f000` = the UBI LEB size). Parsing the UBI volume table proves it:
  `data` = 414 PEBs × 126976 = 52568064 B and `cache` = 281 × 126976 = 35680256 B, matching
  `/proc/mtd` exactly, both with a valid ubifs superblock magic (`31181006`) at LEB 0.

So the gate's literal wording (`--verify` exits 0) is **not achievable on the Elite**. The
substantive gate — every partition's content captured and hash-verified off-device — is met: seven
partitions in both forms, two raw-only, `data`/`cache` inside the verified `ubi` superset. The
manifest keeps recording all four failures; nothing was edited to force a pass. The full analysis,
including how to extract either volume offline with `ubireader`/`nandsim`, is in
`BACKUP-NOTES.md` beside the images.

Delete leftover `*.part` files after reading the manifest; they are empty or partial failures.

## Step 5 `[RO]` — second copy

Skipped by user decision (2026-11-17): the images live on this host's disk only. This closes Phase 1
open question 3 and Phase 3 open question 6 — both are answered "single copy, accepted".

## Step 6 — restore rehearsal

The remote's only restore is the hardware factory reset, so the rehearsal is reading the procedure,
not running it (running it would undo the root we just established and the blank screen we need for
Stage D):

- **Menu + Off held ~20 s**, until the circular-arrow graphic stays on screen. This wipes the
  writable layer: Dropbear, our key, `tdeenable`, `nowatchdog`, `rcS.local`, and the disabled USB
  HID mode all revert, and the stock UI returns.
- Re-rooting after a reset means starting again from Step 1.
- There is **no software restore path** and no UART. Do not create a condition that needs one: never
  flash mtd0–mtd2 (`u-boot env`, `ubl`, `u-boot`) or mtd4/mtd5 (`kernel`, `kernel2`).

## Recovery

| Symptom | Action |
|---|---|
| Blank screen after rooting | Expected. Not a fault. SSH is the interface until Stage D. |
| SSH does not answer | `ip -br link` for `usb0`; re-add `192.168.2.2/24`; replug the bottom port. |
| `usb0` never appears | The remote did not reach the rooted boot path. Menu+Off, then Step 1 again. |
| Remote unresponsive / boot-looping | Menu+Off ~20 s. This is the whole recovery ladder. |
| Anything worse | Stop. There is no UART and no emergency mode on this device. |
