# ha-harmony: custom firmware for Harmony Hub + Elite, HA-native

## Context
Logitech EOL'd Harmony. User owns one Hub + one Elite (latest stock FW: hub 4.15.600, remote 4.15.330), no spare, no UART yet. Goals: full independence from Logitech + new capabilities: HA state on remote screen, every button as HA event, HA drives activities, network devices (Roku/Hue/etc.) via HA. BT v1 targets: Apple TV, Nvidia Shield. Personal use only.
Reference: `iranl/harmony-ng` (MIT) — **clean-room**: read its docs for hardware/RF protocol/rooting facts, write our own code.

Known hardware (from harmony-ng docs):
- Hub: AR9344 MIPS32 BE, Linux 2.6.31, 16MB NOR (mtd0 uboot, mtd1/2 kernel, mtd3 squashfs root, mtd4 /data jffs2, mtd5 /cache), BT via UART HCI `/dev/ttyATH0`, IR via I2S, CC2544 RF via `/dev/rfspi` + stock `cc2544.ko` (SPI shared w/ NOR → single RF owner or hang).

**Corrections from the real rooted hub (verified 2026-11-17, read-only):**
- **The SoC is an Atheros AR9330 "Hornet" with a MIPS 24Kc V7.4 — not an AR9344/74Kc**, and it has
  **no FPU** (`/proc/cpuinfo`; `ASEs implemented: mips16`). The 2.6.31 kernel has no FP emulation, so
  **every floating-point instruction is SIGILL**: hub binaries must be built **soft-float**
  (stock `/usr/bin/hal` is `mips32`, `FP ABI: Soft float`). Our first hard-float cross-build crashed
  six of thirteen C tests on-device and broke the WebSocket handshake. See
  `docs/plans/2026-11-17-phase3-hardware-slice.md` Task 0.
- **IR is emitted as a bitstream, not as pulse timings.** `/dev/i2s` takes a waveform of one bit per
  I2S bit-clock with the carrier cycles already baked in (mark = runs of 1-bits), written into a DMA
  ring and then started by ioctl. Interface documented in `docs/hardware/hub-ir-i2s.md`.
- **7 mtd partitions, not 6** — there is also `mtd6 "mfg"` (64K, all-`0xff`). mtd0 u-boot 64K, mtd1/mtd2 kernel+kernel2 960K each, mtd3 root 4M squashfs, mtd4 data 5M jffs2, mtd5 cache 5M jffs2.
- `/` is **unionfs**: `/mnt/data` (mtd4, jffs2, rw) over `/mnt/root` (mtd3, squashfs, ro). Writes to
  "rootfs" paths such as `/usr/sbin` land in mtd4 and are wiped by the hardware factory reset.
- **BT is not on `/dev/ttyATH0`** — that node does not exist. `hciattach ttyS0 bcsp` attaches it on
  **`/dev/ttyS0`**, exposed as `hci0` with stock `bluetoothd` + `krfcommd`. Our BT plan must target
  `ttyS0`/`hci0`.
- **IR is `/dev/i2s`**; RF is `/dev/rfspi` with `/dev/rffw` alongside (RF firmware). Also present:
  `/dev/caldata`, `/dev/nvram`, `/dev/freset`, `/dev/dk0`/`dk1`, `/dev/gadget`.
- **Contention is real and exclusive:** stock `/usr/bin/hal -f -s` holds `/dev/i2s` open **write-only**
  and `/dev/rfspi` **read-write** for its whole lifetime. Our daemon therefore cannot emit IR or use RF
  while the stock stack runs — the stock `hal` and `lua main.lua` (Luaworks) must be stopped first.
  Nothing we deploy is armed at boot, so a reboot restores full stock behaviour.
- Raw mtd char nodes are named **`/dev/mtdrN`**, not `/dev/mtdNro`, and read identically to `mtdblockN`.
- Stock BusyBox is minimal: **no `dd`, `sha256sum`, `tar`, `find`, `head`, `df`, `sync`, `sort`,
  `readlink`, and no `command` builtin.** `cat`, `md5sum`, `which`, `lua`, `sed`, `grep`, `cp` exist.
- Elite: DM365 ARM926, Linux 2.6.37, 240×320 RGB565 fb, Synaptics touch, MSP430 power/RTC, CC2544 RF, NAND (/ squashfs, /data, /opt).
- RF: HID++-style frames, hub→remote 30B (0x12), remote→hub 32B (0x20); CC2544 FW closed, no RSSI.

**Corrections from the rooted Elite and static analysis of the stock RF stack (2026-11-17):**
- **The Elite has 11 mtd partitions**, not the assumed "squashfs root + /data + /opt": mtd0 `u-boot
  env`, mtd1 `ubl`, mtd2 `u-boot`, mtd3 `ubi` (126M), mtd4/5 `kernel`/`kernel2`, mtd6 `root`, mtd7
  `data`, mtd8 `cache`, mtd9 `mfg`, mtd10 `log`. **mtd4–mtd10 are gluebi volumes inside mtd3**
  (erasesize `0x1f000` = the UBI LEB size), so mtd3 is the superset. Root is unionfs, as on the hub:
  squashfs `/mnt/root` under ubifs `/mnt/data`. See `docs/runbooks/phase1-remote.md`.
- **The 30/32 RF frame split is not directional.** 30 B is the `0x12` report on the *hub's* SPI in
  both directions (the hub's CC2544 runs host mode); 32 B is the `0x20` report on the *remote's* own
  SPI. Both carry the same 28-byte inner block, which is the real application budget.
- **`/dev/rfspi` has no ioctls at all** — only open/read/write/release. `write()` returns the
  *received* frame length rather than the bytes written, `read()` ignores its `count` argument, and
  `read()` blocks with no timeout (a signal is the only way out). Pairing state lives inside the
  CC2544, not in a hub file. The SPI bus is shared with NOR flash, so imaging flash contends with
  RF. Full analysis and the on-device probe set: `docs/hardware/hub-rf-cc2544.md`.
- **Decision: our RF inner layout follows stock's** (`hdr(kind|flags|seq), type, flags,
  payload[25]`) instead of the invented header in `hub/rf/framing.h`, for stock-remote interop and
  so captured stock frames can validate our codec. Payload budget is 24–25 B, not 28.
- **Remote UI is LVGL as planned, but the stock UI is gone**: rooting removes `/data/remote_ui`, so
  the Elite's screen stays blank until our own app is deployed. The remote is out of service for the
  living room from Phase 4 Stage A until Stage D.
- Root: USB HID path (no soldering), stock root untouched, factory reset restores.

## Current setup (verified 2026-11-16, read-only)
- USB: hub `046d:c129` (bcd 11.06), Elite `046d:c12b` (bcd 11.11, shares Touch/Ultimate PID). Each one HID iface, interrupt EP 0x81 IN / 0x02 OUT, 64B. Kernel's HID ignore-list leaves them unbound → Phase 1 needs a udev rule (plugdev) for libusb access.
- HA stock `harmony` integration: hub "Living Room", FW **4.15.600** ✓. Remote FW **4.15.330** ✓.
- Devices: Samsung TV, NVIDIA Shield ("Game Console", BT), KVM 1, J-Tech AV switch, Roku stick, Audioengine A5+. Activities: "SHIELD TV", "Watch Roku".
- HA consumers: `remote.harmony`, `select.living_room_activities`, `switch.living_room_watch_roku` + 7 automations/scripts/scenes/helpers referencing harmony → migration must map or preserve these.

## Decisions
- **Model: middle-ground hub.** HA owns config/logic; HA compiles button→IR/BT mapping tables + activity steps and pushes them to hub; hub executes locally (instant, works when HA down), reports every press/gesture to HA as events. Non-IR/BT actions go through HA.
- **Stock kernels v1**, userspace only, **C11** static (uClibc, Bootlin toolchains). New hub kernel (OpenWrt ath79 + own CC2544/IR drivers) = later phase, requires USB-UART adapter.
- **HA link:** custom integration (HACS-style `custom_components/`) ↔ hub WebSocket (token auth, mDNS discovery).
- **Remote UI:** pages defined in HA (YAML/options flow: entity tiles, button grids, now-playing, activities), pushed to remote, rendered natively with **LVGL** on fbdev.
- **BT:** BTstack library (personal-use licence OK) over `/dev/ttyATH0`, HID device role for Apple TV/Shield.
- **IR sources:** extract from stock hub backup, import Flipper IRDB/SmartIR, learning (verify hub has IR RX; else external receiver).
- **Brick safety:** only touch /data,/cache (hub) and /data,/opt (remote); never uboot/kernel/root in v1. Full partition backups before anything. Strongly recommend buying a USB-UART adapter before Phase 1.

## Repo layout
- `proto/` — versioned message schema; JSON over WS (hub↔HA), compact binary w/ fragmentation over RF (≤30/32B frames). Single source → generated C + Python.
- `hub/` — daemon: `rf/` (sole owner of `/dev/rfspi`), `ir/` (I2S out, Pronto/NEC/RC5/RC6/Sony encoders), `bt/` (BTstack HID), `engine/` (mapping tables, activity state machine, persisted cache on /data), `net/` (WS server, mDNS), `ota/` (signed packages).
- `remote/` — app: LVGL page renderer, keys/touch/accelerometer input, RF client, MSP430 power/sleep, local fallback pages.
- `custom_components/ha_harmony/` — config flow + zeroconf, WS coordinator, entities (remote, activity select, event entity per remote, buttons, sensors), page editor, IR library import/learn, mapping compiler + push, services.
- `sim/` — host builds of hub & remote with fake RF/IR/BT; remote UI via LVGL SDL. Enables dev/tests without hardware.
- `tools/` — clean-room USB root, full backup (all mtd/NAND + sha256), IR code extraction from stock data, deploy, restore-stock.
- `docs/` — design spec (`docs/design/2026-11-16-ha-harmony-design.md`), our own hardware/protocol notes.

## Phases
0. **Spec + notes**: commit design spec; write own hardware/RF/rooting notes from harmony-ng docs. Order USB-UART adapter.
1. **Root + backup** (hub, then remote): root via USB, dump every partition, verify hashes offline, extract stock IR codes + device/activity config to JSON. Gate: verified backups + tested restore-stock.
2. **Foundations**: `proto/` + codegen, `sim/` hub/remote, HA integration skeleton talking to sim hub.
3. **E2E slice (MVP)**: HA pushes a page with one button → remote renders → press goes over RF → hub fires IR locally + emits HA event. On real hardware.
4. **Fill out**: activities (HA-defined, hub-executed, power-state tracking), BT HID to Apple TV/Shield, IR import/learn, HA entity tiles + now-playing on remote, network-device actions via HA, HA-offline fallback, signed OTA (hub→remote over RF), remote sleep/wake.
5. **Later**: new hub kernel (needs UART).

## Risks
- Elite untested in harmony-ng (only Ultimate) → expect remote quirks; dev on remote after hub stable.
- Rooting depends on exact latest stock FW — don't let it update/factory-change before backup.
- `/dev/rfspi` concurrency can hang flash bus → single RF thread, enforced in code.
- Remote can't be RF-woken while asleep → wake via MSP430/buttons/accelerometer only.
- No UART = limited recovery → strict partition scope, boot-loop counter → safe mode.

## Verification
- C: host unit tests (cmocka) for encoders, RF framing, mapping engine, proto; sanitizers on host builds.
- Python: `pytest -n auto` with `pytest-homeassistant-custom-component`; xdist-safe (per-worker tmp/ports).
- `sim/` integration: HA test instance ↔ sim hub ↔ sim remote E2E.
- CI: GitHub Actions — cross-builds (MIPS/ARM) + test matrix shards + single aggregator check.
- Hardware: IR verified with a phone camera/IR receiver; RF via sim→real swap; HA via HA MCP (entities, events, traces).

## Left to builder
Exact WS/RF message set, page YAML schema, LVGL widget set, OTA signing scheme, mDNS service name, integration domain name.
