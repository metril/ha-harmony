# ha-harmony: custom firmware for Harmony Hub + Elite, HA-native

## Context
Logitech EOL'd Harmony. User owns one Hub + one Elite (latest stock FW: hub 4.15.600, remote 4.15.330), no spare, no UART yet. Goals: full independence from Logitech + new capabilities: HA state on remote screen, every button as HA event, HA drives activities, network devices (Roku/Hue/etc.) via HA. BT v1 targets: Apple TV, Nvidia Shield. Personal use only.
Reference: `iranl/harmony-ng` (MIT) — **clean-room**: read its docs for hardware/RF protocol/rooting facts, write our own code.

Known hardware (from harmony-ng docs):
- Hub: AR9344 MIPS32 BE, Linux 2.6.31, 16MB NOR (mtd0 uboot, mtd1/2 kernel, mtd3 squashfs root, mtd4 /data jffs2, mtd5 /cache), BT via UART HCI `/dev/ttyATH0`, IR via I2S, CC2544 RF via `/dev/rfspi` + stock `cc2544.ko` (SPI shared w/ NOR → single RF owner or hang).
- Elite: DM365 ARM926, Linux 2.6.37, 240×320 RGB565 fb, Synaptics touch, MSP430 power/RTC, CC2544 RF, NAND (/ squashfs, /data, /opt).
- RF: HID++-style frames, hub→remote 30B (0x12), remote→hub 32B (0x20); CC2544 FW closed, no RSSI.
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
