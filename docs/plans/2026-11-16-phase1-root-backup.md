# Phase 1: Root + Full Backup (Hub, then Remote)

Spec: `docs/design/2026-11-16-ha-harmony-design.md` (Phase 1 + Risks).

**Goal:** root SSH on the hub, then the remote; capture full partition images with sha256 verified
on both ends plus the external tool's tar backups; verify offline; extract stock device/activity/IR
config to JSON. This is the gate for Phase 3 hardware work.

**Approach (user decision, 2026-11-16):** the external root tool — `iranl/harmony-ng`, MIT, read-only
clone at `/home/user/projects/upstream-ref/harmony-ng` — is *run as a tool* to obtain
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
| `tools/hubfind.py` | todo | hub IP discovery + TCP 8088/22 reachability report |
| `tools/extract_stock.py` | todo | stock backup → `devices.json`, `activities.json`, `ir_codes.json` |
| `docs/runbooks/phase1-hub.md`, `phase1-remote.md` | todo | by-hand procedure, abort points, recovery |

Backups live outside git: `backups/phase1/<device>/<utc-timestamp>/` (gitignored), `*.img` +
`manifest.json`.

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

### Task 2 `[RO]` — Hub discovery and reachability

The external root tool aborts if the host cannot reach the hub on 8088/22 *after* rooting — which
would cost us the rooting window, not the device. Known: host is `198.51.100.126/24` on `enp5s0`; a
sweep of 8088 across `198.51.100.0/24` found nothing, so the hub is on another subnet/VLAN. HA reaches
it (entry `c421d657ea8e44d7a66510b46f4dbd8c`, unique_id `REDACTED-HUB-ID`, sw 4.15.600) but HA diagnostics do
not expose the IP — it lives in HA's `.storage/core.config_entries`, or the router's ARP/DHCP table.

- [ ] `tools/hubfind.py`: parse `ip neigh`/`arp -an` for Logitech OUIs, optional mDNS/SSDP, rank
      candidates, probe 8088/22 via an injected `probe(host, port)` (tests use no real sockets).
- [ ] **User supplies** the hub IP, then one of: **(a)** routing — VLAN id, subnet, gateway, and
      whether inter-VLAN ACLs permit 8088/22 from this host (preferred: no device writes); **(b)**
      re-point the hub's Wi-Fi via the tool's `--wifi-ssid/--wifi-pass` — this is a **`[WRITE]`** that
      reboots the hub and discards its stock Wi-Fi config; **(c)** run the external tool from a host
      already on the hub's subnet (needs moving the USB cable) — only if (a) and (b) fail.
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

## Open questions for the user

1. **Hub IP + reachability** — the IP, and option (a) routing (VLAN id, subnet, gateway, ACLs) or
   (b) Wi-Fi re-point (SSID, PSK, encryption)? Also: confirm the hub can be blocked from WAN during
   Phase 1.
2. **NAND imaging fidelity (remote)** — `cat`/`dd` of `/dev/mtdN` gives main-area data only: no
   OOB/ECC spare, bad-block info lost. Accept main-area-only, or require `nanddump --oob` (and accept
   staging a tool if stock BusyBox lacks it)? We never write NAND in v1, so this only matters for
   worst-case restore fidelity.
3. **Remote `/data` purity** — step 2 backs up and then installs upstream's runtime in one shot.
   Accept a `/data` image containing upstream files (the tar holds stock content), interrupt the tool
   after its backup (risky), or factory-reset + re-root afterwards to image a pristine `/data`?
4. **Off-host backup destination** — hub ~16 MB, remote ~112 MB plus manifests.
5. **UART adapter** — confirm it is true 3.3 V TTL (never 5 V on J2).

## Testable without hardware

All of Task 3 and the parsers in Tasks 2 and 6: fake-SSH transport, `/proc/mtd` and
`/proc/partitions` parsing, capability probing, dump + dual hashing + manifest, truncated streams,
the brick-safety command-log assertion, offline verify (byte flip, missing/extra image, filesystem
magic), candidate ranking with an injected port probe, and stock-config extraction against fixtures
cross-checked with HA's live stock integration view.
