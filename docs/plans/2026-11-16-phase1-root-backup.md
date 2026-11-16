# Phase 1: Root + Full Backup (Hub, then Remote)

Spec: `docs/design/2026-11-16-ha-harmony-design.md` (Phase 1 + Risks).

**Goal:** root SSH on the hub, then the remote; capture full partition images with sha256 verified
on both ends plus the external tool's tar backups; verify offline; extract stock device/activity/IR
config to JSON. This is the gate for Phase 3 hardware work.

**Approach (user decision, 2026-11-16):** the external root tool — `iranl/harmony-ng`, MIT, read-only
clone at `/home/OLYMPOS/jagannath/projects/upstream-ref/harmony-ng` — is *run as a tool* to obtain
root SSH and its tar backups. None of its code enters our firmware; our firmware stays clean-room.
Our repo gains host preflight, hub discovery, full-image backup/verify, stock-config extraction, and
two runbooks.

Every task is marked `[RO]` (read-only on the devices) or `[WRITE]`. **A `[WRITE]` task starts only
on the user's go-ahead at the moment it runs.**

## Global constraints

- Host tooling is Python 3.14, stdlib only, `uv`, `pytest -n auto` (xdist-safe: `tmp_path`, no shared
  state), `ruff` line-length 100. No C this phase.
- **Never write hub `mtd0` (U-Boot), `mtd1`/`mtd2` (kernels), `mtd3` (squashfs root), nor the
  remote's rootfs.** Our tools are read-only on the device by construction: no `dd of=`, no
  `nandwrite`, no `flash_erase`, no redirect into `/dev/mtd*` anywhere in our code — asserted by a
  test over the emitted command log.
- Devices must not take a stock firmware update before backup: always `--skip-fw-check` (both are
  already at the targeted versions — hub 4.15.600, remote 4.15.330). Do not open MyHarmony or the
  Harmony app during Phase 1.
- Recovery ladder: software restore → hardware factory reset (hub: hold Pair while plugging USB,
  15–20 s, release on fast red; remote: Menu+Off ~20 s until the circular-arrow graphic stays on) →
  USB-UART on hub J2 (pin 2 RX, 4 TX, 8 GND, **3.3 V only**, 115200 8N1).
- **Hub completely done and gated before the remote is touched.** The remote has no documented UART
  path, so its only fallback is the hardware reset.

## Files

| Path | State | Responsibility |
|---|---|---|
| `tools/udev/99-ha-harmony.rules` | done | plugdev access for usb + hidraw nodes |
| `tools/host_setup.sh` | done | installs the rule + `usbhid` `HID_QUIRK_NO_IGNORE`, rebinds |
| `tools/device_backup.py` | done (`phase1-backup-tool`) | discover partitions, stream images, dual hash, manifest |
| `tools/device_backup.py --verify` | todo | offline re-hash of an image set against its manifest |
| `tools/extract_stock.py` | todo | stock backup → `devices.json`, `activities.json`, `ir_codes.json` |
| `docs/runbooks/phase1-hub.md`, `phase1-remote.md` | todo | by-hand procedure, abort points, recovery |

Backups live in-tree but gitignored (user's choice): `backups/phase1/<device>/<utc-timestamp>/`,
holding `*.img` + `manifest.json`. `backups/` is in `.gitignore`. Note this keeps the only copy on
this host's disk — worth a second copy elsewhere before Phase 3 starts writing to hardware.

---

### Task 1 `[RO]` — Host preflight: can we talk HID at all?

The one unknown that can stop Phase 1 dead. Verified starting state: both devices present with HID
interface class but `Driver=[none]` and **no hidraw node** (`hidraw0..2` belong to an unrelated
keyboard). The external tool scans `/sys/class/hidraw`, so as-is it reports "not detected". The
kernel's HID ignore list covers the Harmony PIDs; `/sys/module/usbhid/parameters/quirks` is empty, so
the `HID_QUIRK_NO_IGNORE` (`0x40000000`) override is available.

- [x] udev rule covers both `SUBSYSTEM=="usb"` and `SUBSYSTEM=="hidraw"` for `046d:c129`/`c12b`.
- [x] `tools/host_setup.sh` installs the rule, sets the quirks (sysfs now + `modprobe.d` for
      persistence), rebinds the interfaces, and prints the resulting hidraw nodes. `--undo` reverses it.
- [ ] **User runs** `sudo tools/host_setup.sh`; its output decides the next step.
- [ ] If hidraw appears for both PIDs → Task 2. If not: fall back to running the external tool as
      root, and if that also fails, **stop and report** — do not improvise kernel patches.
- [ ] Rollback: `sudo tools/host_setup.sh --undo`. Nothing on the devices changed.

### Task 2 `[RO]` — Hub reachability — **DONE, no writes needed**

The external root tool aborts if the host cannot reach the hub on 8088/22 *after* rooting — which
would cost us the rooting window, not the device.

- [x] **Hub is `10.10.76.11`** (user-supplied). Host is `10.10.77.126/24` on `enp5s0`; the hub is on
      another subnet but **already routed** via `10.10.77.1` — no VLAN work, no Wi-Fi re-point, so
      reachability costs zero device writes. Verified read-only, 2026-11-16: ping 2/2, **8088 open**,
      **5222 open** (stock XMPP), **22 closed** (expected pre-root). A `setup.account?getProvisionInfo`
      POST to 8088 returned `activeRemoteId REDACTED-HUB-ID`, matching HA's `harmony` entry unique_id — so
      `10.10.76.11` is confirmed as this hub, not another device.
- [x] `tools/hubfind.py` **dropped** — the IP is known and routing works; a discovery tool would be
      dead code. Re-add only if the hub's address changes.
- [ ] **Isolate the hub from WAN** for the rest of Phase 1 (MAC block or offline SSID) so it cannot
      pull a stock update. Belt-and-braces: it is already at the latest version.
- [ ] Gate: from this host, 8088 open and 22 closed (stock). Do not start Task 4 otherwise.

### Task 3 `[RO]` — Backup + verify tooling (no hardware needed)

- [x] `tools/device_backup.py` + `tests/py/test_device_backup.py`: `/proc/mtd` discovery with
      `/proc/partitions` fallback, capability probing, stream to `.part` → `.img`, device-side and
      host-side hash compared (`sha256sum`, `md5sum` with a loud warning), manifest on success *and*
      failure, skip-complete, `--only`, `--dry-run`. 13 tests, all against a fake SSH transport.
- [ ] Harden before hardware (gaps found in review of the first cut):
  - [x] SSH timeouts (`ConnectTimeout`, `ServerAliveInterval`/`CountMax`) so a hung hub cannot hang a run.
  - [x] `cat /dev/mtdNro` read path for a BusyBox without `dd` — the remote's stock BusyBox is
        reported to lack it, and today a missing `dd` is a hard failure.
  - [x] Brick-safety test: no emitted command may contain `of=`, `nandwrite`, `flash_erase`, or a
        redirect into `/dev/mtd*`.
  - [x] `--verify <dir>`: offline re-hash of every image against both manifest hashes, size check,
        and filesystem-magic report (squashfs/jffs2/ubi); non-zero on any mismatch, missing or extra image.
- [ ] Gate: full suite green (397 pre-existing + new), `ruff check` clean.

### Task 4 `[WRITE — go-ahead needed at the moment it runs]` — Root the hub

What it changes, all on `mtd4` (`/mnt/data`) — which is exactly what the hardware factory reset
wipes, hence recoverable without UART: stages Dropbear via `/sys/dropbear`, arms `/etc/tdeenable`,
empties the root password, writes `/etc/nowatchdog`, reboots.

- [ ] User runs, from the upstream clone:
      `python tools/root_hub_usb.py --hub-ip <HUB_IP> --skip-fw-check --pubkey ~/.ssh/harmony_owner_ed25519.pub`
      (add `--wifi-ssid/--wifi-pass` only if Task 2 chose (b)). **Do not** pass `--no-backup`; we want
      its tar. Do not run its `deploy_hub.py` in Phase 1.
- [ ] Gate: `ssh -i ~/.ssh/harmony_owner_ed25519 root@<HUB_IP> "uname -a; id; cat /proc/mtd"` works,
      and `backups/stock_hub_backup_*.tar` + `golden_hub_backup.tar` exist at plausible sizes. If SSH
      came up but the tar is missing, run its `tools/backup_hub.py <HUB_IP>` before anything else.
- [ ] Rollback: hub still boots stock → hardware factory reset, then retry. Boot-loop that reset
      cannot fix → UART J2. **Keep the UART adapter connected and a terminal ready for this task.**
- [ ] `docs/runbooks/phase1-hub.md` records the exact commands run and their output.

### Task 5 `[RO]` — Full hub images + offline verification

- [ ] `uv run tools/device_backup.py --target hub --host <HUB_IP> --key ~/.ssh/harmony_owner_ed25519
      --out-dir backups/phase1/hub/$(date -u +%Y%m%dT%H%M%SZ)` → mtd0..mtd5.
- [ ] `uv run tools/device_backup.py --verify backups/phase1/hub/<ts>` → every partition matches,
      sizes equal `/proc/mtd`, magics plausible.
- [ ] Copy the image set **and** the external tool's tars off-host; re-hash after transfer. One copy
      on one disk is not a backup.
- [ ] A partition that cannot be read is recorded explicitly — never a silent gap.

### Task 6 `[RO]` — Extract stock config and IR codes

- [ ] `tools/extract_stock.py`: stock tar / loop-mounted image → `devices.json`, `activities.json`,
      `ir_codes.json` under `backups/phase1/hub/<ts>/extracted/`. Unknown IR encoding is recorded raw
      with a warning, never dropped; a device with no commands is an error.
- [ ] Gate: names and command counts match HA's live stock view — 6 devices (Samsung TV `51886472`,
      NVIDIA Game Console `67368870`, KVM 1 `81729894`, J-Tech AV Switch `64694265`, Roku Streaming
      Stick `80756020`, Audioengine A5+ `51887071`), activities `42831432` "SHIELD TV", `52417136`
      "Watch Roku", `-1` PowerOff.

### Task 7 `[RO]` — Restore rehearsal (dry run)

- [ ] Run the external `tools/restore_stock.py --skip-usb-root --hub-ip <HUB_IP> --backup
      backups/stock_hub_backup_latest.tar` and **abort at its typed-`RESTORE` prompt**; record the
      exact command.
- [ ] Document the three-tier ladder in the hub runbook, with the caveats: a factory-reset hub has no
      BusyBox `tar`, and a fresh root run overwrites `stock_hub_backup_latest.tar` with a backup of
      the *reset* hub — always restore from the dated/golden tar.
- [ ] **Hub gate:** verified images off-host + tars + extracted JSON + a reviewed restore procedure.
      Do not touch the remote until this passes.

### Task 8 `[WRITE — go-ahead needed]` — Root the remote

Via the bottom micro-USB port (not the cradle). Step 1 permanently disables the remote's USB HID
mode, stops the stock UI, replaces `/etc/init.d/rcS.local` with a USB-Ethernet + SSH boot script,
removes `/data/remote_ui`, writes `/etc/nowatchdog`. All in the writable layer → Menu+Off wipes it.
**The screen stays blank until something is deployed; that is expected, not a brick.**

- [ ] Host side `[RO]`: after re-enumeration, `sudo ip addr add 192.168.2.2/24 dev <usb-if>;
      sudo ip link set <usb-if> up` — **no default route** on that interface.
- [ ] Build the ARM BusyBox in the upstream clone first (`./build/build_busybox.sh arm`) or its
      backup silently does not happen (stock BusyBox lacks `tar`).
- [ ] `[WRITE]` `python remote/setup_remote.py --step 1 --skip-fw-check`, then `--step 2 --skip-fw-check`.
      Step 2 backs up *and then* installs upstream's `remote_ui` into `/data` in one shot — see open
      question 3.
- [ ] Gate: `ssh -i ~/.ssh/harmony_owner_ed25519 root@192.168.2.1 "uname -a; cat /proc/mtd; mount"`
      works; `backups/remote/*_backup_*.tar` + `golden_remote_backup.tar` exist.
- [ ] Rollback: Menu+Off ~20 s → full wipe of the writable layer, back to stock.

### Task 9 `[RO]` — Full remote images + Phase 1 exit gate

- [ ] `uv run tools/device_backup.py --target remote --host 192.168.2.1 ... --out-dir
      backups/phase1/remote/<ts>`; the tool reads whatever layout the device reports (mtd or
      `/proc/partitions`; squashfs root + `/data` + `/opt`, or a UBI volume layout) rather than
      hard-coding either.
- [ ] `--verify` → 0; copy off-host; re-hash after transfer.
- [ ] **Exit gate:** both image sets verified and off-host; both tars; extracted stock JSON; both
      runbooks written; `uv run pytest -n auto` and `uv run ruff check .` green.

---

## Decisions recorded (2026-11-16)

- **Hub reachability:** `10.10.76.11`, already routed — Task 2 done, no device writes. ✓
- **NAND imaging (remote):** main-area only is accepted. No `nanddump --oob`, nothing staged onto the
  remote to achieve it. Bad-block/OOB info is therefore not captured; acceptable because v1 never
  writes NAND and the hardware reset is the real recovery path. ✓
- **Remote `/data` purity:** accept a `/data` image containing the external tool's runtime; its tar
  holds the stock `/data` content, and rootfs//opt images stay pristine. No interrupt, no
  reset-and-re-image cycle. ✓
- **Backup location:** in-tree, gitignored `backups/phase1/...`. ✓

## Still open

1. **WAN isolation** — confirm the hub can be blocked from the internet (MAC filter / offline SSID)
   for the duration of Phase 1 so it cannot pull a stock update. It is already at the latest
   version, so this is belt-and-braces, not a gate.
2. **UART adapter** — confirm it is true 3.3 V TTL (never 5 V on hub J2), and plug it in before
   Task 4: nothing is on `/dev/ttyUSB*` yet and no terminal emulator (`picocom`) is installed.
3. **Second backup copy** — the images will live only on this host's disk; decide where a second
   copy goes before Phase 3 writes to hardware.

## Testable without hardware

All of Task 3 and the parsers in Task 6: fake-SSH transport, `/proc/mtd` and `/proc/partitions`
parsing, capability probing, dump + dual hashing + manifest, truncated streams, the brick-safety
command-log assertion, offline verify (byte flip, size mismatch, missing/extra image, leftover
`.part`/`.CORRUPT`, filesystem magic), and stock-config extraction against fixtures cross-checked
with HA's live stock integration view. 429 tests green as of 2b6b21c.
