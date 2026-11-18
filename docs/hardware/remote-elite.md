# Harmony Elite remote: hardware interfaces

What our remote firmware has to drive, from a read-only survey of the rooted Elite (live over SSH and
from the verified backup) and the Task 0 probe session on 2026-11-17.

Tags: **[MEASURED]** = run on the device with our own probe; **[CODE]** = read from a stock binary
or driver; **[INFERRED]** = conclusion not directly observed.

Device: "Logitech Juniper", TI DA850/DM365, ARM926EJ-S (ARMv5TEJ, no VFP), ~149 BogoMIPS, Linux
2.6.37-ge384483 PREEMPT. 61.7 MB RAM, ~50 MB usable with the stock stack off, no swap.

## Hard rules

- **Toolchain must be musl.** uClibc-ng 1.0.54 gets ENOSYS from `stat`, `fstat`, `opendir` and
  `clock_gettime` on 2.6.37 (it calls `statx` and `clock_gettime64` with no fallback). musl retries
  the old syscalls. **[MEASURED]** Static linking only: stock is uClibc 0.9.30.1.
- **Never read the mounted gluebi volumes** `mtd7`/`mtd8` (`data`, `cache`): it oopses the kernel in
  `ubi_leb_read` and leaves unkillable D-state readers. **[MEASURED]**
- **`/dev/rfspi`: never `write()` more than 32 bytes** — the driver `memcpy`s `count` into a 32-byte
  slab (heap overflow). **[CODE]** Keep a reader running: received frames queue in kernel heap even
  with no opener.
- **Never open** `/dev/rffw` (reflashes the CC2544), `/dev/mspfw` (reflashes the MSP430),
  `/dev/watchdog` (arms a 10 s reset); never write `1-0034/kill` or the PMIC `charger` node.
- **Battery:** stock powered off on low charge; nothing does now. Keep the Elite on USB/cradle until
  our battery guard runs.

## Display **[MEASURED]**

`/dev/fb0`, "DA8xx FB Drv", ILI9341 panel. `xres=240 yres=320`, `virtual=240x960` (three stacked
pages — y-pan double-buffering is available), `bpp=16`, **RGB565**: red offset 11 length 5, green
offset 5 length 6, blue offset 0 length 5, no alpha. `line_length=480`, `smem_len=466944`. No
rotation. Stock (`luadraws.so`, cairo/pango) mmaps fb0.

**The panel stays dark until `FBIOBLANK(FB_BLANK_UNBLANK)` is issued [MEASURED, bench 2]:** drawing into the
mapping and panning change nothing visible; `echo 0 > /sys/class/graphics/fb0/blank` made our framebuffer
appear. The app issues it right after mapping (`ra_fb_unblank`, the only blank request the allow-list admits).

Backlight `/sys/class/backlight/ap3156_backlight/`: `max_brightness` 31, stock maximum 26, fades
1 step per 50 ms to dim/off. Key LEDs `/sys/class/leds/tps6507x:white/brightness`, max 99, stock 42,
written 0 before suspend. **[CODE]**

## Input **[MEASURED]**

| node | name | role |
|---|---|---|
| `event0` | `msp430` | hard keys, IRQ 132 |
| `event1` | `LIS3DH accelerometer` | motion, IRQ 108 |
| `event2` | `Synaptics Clearpad` | touch + Home/Favorites strip, IRQ 118 |
| `event3` | — | stale node, `No such device` |

**Touch** (`event2`), single-touch, no `ABS_MT`: `ABS_X` 0–239, `ABS_Y` 0–320 (fuzz 3),
`ABS_PRESSURE` 0–255, `ABS_TOOL_WIDTH` 0–15, `BTN_TOUCH` (330). The driver turns touches below the
screen into keys itself: far left → `KEY_HOME` (102), far right → `KEY_FAVORITES` (364), the middle
emits nothing. So the strip arrives as keys on `event2`, not as coordinates.

**Hard keys** (`event0`) — all 35 confirmed by pressing each one; the names are stock's:

| group | evdev code → stock name |
|---|---|
| nav | 103 Up, 108 Down, 105 Left, 106 Right, 352 Select |
| menu | 139 Menu, 174 Exit |
| volume | 115 VolumeUp, 114 VolumeDown, 113 VolumeMute |
| channel | 402 ChannelUp, 403 ChannelDown, 412 PrevChannel |
| transport | 207 Play, 119 Pause, 128 Stop, 167 Record, 168 Rewind, 159 FastForward |
| guide | 389 DVR, 395 Guide, 358 Info |
| colours | 398 Red, 399 Green, 400 Yellow, 401 Blue |
| home control | 148 HomeControl1, 202 HomeControl2, 149 HomeControl3, 203 HomeControl4, 78 HomeControlPlus, 74 HomeControlMinus |
| power | 116 PowerOffActivity |
| strip (`event2`) | 102 Home, 364 Favorites |

There are **no numeric keys** and no Back key (it is Exit). Codes run to 412, so the integration's
logical key ids must be a mapping, not the evdev code.

**IRQ counters are zero until something is pressed** — a quiet `/proc/interrupts` is not a dead
input path. (A first 90 s capture saw nothing because nothing was pressed in that window.)

## RF **[MEASURED]**

`/dev/rfspi` (10:59, misc `cc2544_spi`), module `cc2544.ko` md5 `5b9979da8d9648902f7eb5b3464f0b85`,
chip id `0x4415`, real SPI controller (`spi_davinci.1`, IRQ 112), not the hub's bit-banged bus.
Same semantics as the hub (`docs/hardware/hub-rf-cc2544.md`): no ioctls, `write()` returns 0 for a
command and the reply arrives on `read()`, `read()` ignores `count`.

- Firmware **72.00.0007**, matching `/lib/firmware/cc2544.version` (the hub's chip is 39.00.0010 —
  different firmware for host and device roles).
- Pairing is intact; no re-pair is needed. The remote's own pipe table:

| pipe | payload | reading |
|---|---|---|
| `0x03` | `75 BB 13 86` | own RF address — the hub lists it in its slot 1 |
| `0x20` | `08 28 88 0B 04 01 07 A1 B7 95 EE` | host entry, ending in the hub's address |
| `0x30` | `A1 B7 95 EE …` | **hub RF address** `A1B795EE` |
| `0x40` | `01 07 "Juniper"` | name |

The remote sees the hub in its slot 0; the hub sees the remote in its slot 1.

Remote-mode frames are `0x20, (slot & 0xF) | powermode, 0x12, slot, inner[28]` = 32 bytes. **[CODE]**
Stock sends a key press as an application message (`control.button?press|hold|release` with the
stock key name) over its HOT object layer, not as a dedicated frame. **[CODE]**

## Power, sleep and battery **[CODE]** unless noted

- PMIC `tps6507x` at `1-0048`; its nodes are on the **`tps6507x-juniper` sub-device**:
  `/sys/bus/i2c/devices/1-0048/tps6507x-juniper/{powersource,vbat,battcharge,charger,…}`.
  `powersource` ∈ `battery|cradle|wall_adapter|usb` (reads `usb` now), `vbat` "4.17 0x02c9",
  `battcharge` "0.00 0x0000" (stock: > 0.3 A means charging). **[MEASURED]**
- Gauge `stc3115` at `1-0070`: `soc` (reads 100.0), `vbat`, `cur`, `ocv`, `temperature`.
  **[MEASURED]**
- MSP430 at `1-0034`: `wakeup` (seconds to RTC alarm), `wakeup_event` (`none|key|alarm|touch`),
  `version` 202, `button`, `status`, `time`, `alarm`, `bootcmd`; `kill` — never touch.
- Stock sleep: fade backlight → skip if on USB → write seconds to `1-0034/wakeup` → write `mem`
  (battery) or `standby` (cradle) to `/sys/power/state` (blocks until wake) → read `wakeup_event`.
  Untested by us; the riskiest remote write.
- Haptics `/dev/drv2605`; IR emitter `/dev/ir`.

## Boot

`inittab` runs `/etc/init.d/rcS`, which **sources** `/etc/init.d/rcS.local`. Ours (from the root)
ends in `exit 0`, so `rcS` stops there: no `hal`, no stock UI, no watchdog daemon. Current
processes: init, syslogd, klogd, dropbear. `/etc` and `/data` write through the unionfs to ubifs and
persist; `/var/volatile` is tmpfs. The boot hook is `tools/deploy_remote.py install-hook`.

**rcS.local probe (RO, 2026-11-18) [MEASURED]:** `/` is `none on / type unionfs (dirs=/mnt/data=rw:/mnt/root=ro)`
(rw branch `ubi0:data` ubifs, ro branch squashfs `mtdblock6`); the branch mounts are not visible
in the running namespace, so whether `rcS.local` sits in the rw layer is not directly observable
(it is root-edited, 117 B, mode 755). `rcS` runs `if [ -x /etc/init.d/rcS.local ]; then source ...`:
**sourced and `-x`-guarded**, so a syntax error aborts `rcS` itself (selftest INFO line: the caller
printed nothing afterwards) and a non-executable file skips it, taking our dropbear start with it
(`rcS` has a later `-r /etc/tdeenable` fallback to `/usr/sbin/dropbear`, which exists; untested as a
recovery). **Rename atomicity on this unionfs-over-ubifs is UNKNOWN**; `deploy_remote.py` says so
before every rcS.local write and gates it with an on-device `sh -n`, md5 read-back and restore.
BusyBox 1.13.4 here has no `head tail wc dd df find tar sync basename od`; it does have `sh -n`
(verified to reject a broken file), `md5sum`, `pidof`, `kill`, `cp`, `mv`, `chmod`, `awk`, `cut`.

The kernel watchdog driver is loaded but **not armed** (nothing opens `/dev/watchdog`; the device
has run for hours unfed). `/etc/nowatchdog` also stops `rcS` starting the stock watchdog daemon.

`/data` (ubifs, 405 LEBs ≈ 49 MB) has roughly 45 MB free **[INFERRED]** from mapped LEBs; there is
no `df` on the device.
