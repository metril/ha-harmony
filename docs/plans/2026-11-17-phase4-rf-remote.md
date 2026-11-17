# Phase 4 Stage C+D: real RF and the remote app

Spec: `docs/design/2026-11-16-ha-harmony-design.md`. RF facts: `docs/hardware/hub-rf-cc2544.md`.
Remote facts: `docs/hardware/remote-elite.md` (Task 0 writes it from the 2026-11-17 recon).
Stages C (RF transport) and D (remote app) are **merged** (user, 2026-11-17): the rooted Elite cannot
transmit until it runs our firmware, so the two share one hardware gate.

**Gate (one bench window):** HA pushes a page → the Elite renders it with our app → a tap and a hard
key each cross **real RF** → the hub fires IR at the TV and HA receives both as events. Then 30 min
idle on the cradle with no leak, crash or watchdog reset.

Every task is `[RO]` or `[WRITE]` with respect to the devices; a `[WRITE]` step runs only on the
user's go-ahead at the time. Host tests: ctest (ASan/UBSan, `-Werror`), `uv run pytest -n auto`,
xdist-safe.

## Hard rules learned this stage

- **Never read the Elite's mounted gluebi volumes** (`mtdblock7` `data`, `mtdblock8` `cache`, or
  their `mtd` nodes). It oopses the kernel in `ubi_leb_read` and leaves unkillable D-state readers.
- **Remote `/dev/rfspi`: never `write()` more than 32 bytes** — the driver `memcpy`s `count` bytes
  into a 32-byte slab with no check (kernel heap overflow). Short writes are zero-padded and fine.
- **Remote RF must always have a reader** while the radio is up: received entries are queued even
  with no opener, ~64 B of kernel heap each.
- **Never open** `/dev/rffw`, `/dev/mspfw`, `/dev/watchdog`, or write `1-0034/kill` / `charger`.
- **Battery:** our app must poll the gauge and `/sbin/poweroff` below a threshold, as stock did.
  Until it does, the Elite stays on USB/cradle.
- Both RF drivers: `write()` returning 0 is success (reply comes via `read()`); `read()` ignores
  `count` (pass ≥ 64 B, we use 128) and blocks until a signal.

## Decisions (made here, override in review if wrong)

- **LVGL v9.2.2**, vendored under `vendor/lvgl/`, its own fbdev and evdev drivers unused: we write a
  ~150-line RGB565 flush to an `mmap`ed `/dev/fb0` (double-buffered by y-pan; virtual height is 960)
  and our own evdev reader, so the dependency stays render-only. **No SDL**: host tests render pages
  into a memory framebuffer and compare a hash against golden vectors.
- **Static linking on the remote, with musl** (Bootlin `armv5-eabi--musl--stable-2025.08-1`). Not
  uClibc-ng: measured on the Elite 2026-11-17, uClibc-ng 1.0.54 binaries get **ENOSYS** from `stat`,
  `fstat`, `opendir` and `clock_gettime`, because it calls `statx` (Linux 4.11+) and
  `clock_gettime64` (5.1+) with no fallback, and the Elite runs 2.6.37. musl retries the old syscalls
  on ENOSYS; the same test passes with it. Dynamic linking against stock uClibc 0.9.30.1 cannot work
  either. `remote-arm.cmake` refuses a non-musl sysroot.
- **RF object layer is ours, inner layout is stock's.** Inner block: `hdr = kind(0x20 object,
  0x40 ack, 0x60 continuation) | 0x10 on an object's first frame | seq(4 bit)`, `type`, `flags`,
  `payload[25]`. Objects are `len(1) + body`, split into 25-byte chunks, **stop-and-wait with one
  ack per object**, 3 retries at 150 ms. Simple beats windowed for ≤255-byte pages.
  - **No CRC:** the radio already guarantees integrity and stock frames carry none, so a CRC would
    stop captured stock frames validating our codec. Add one only if the bench shows corruption.
  - **Duplicate suppression:** separate seq counters per direction, and the receiver keeps the last
    delivered seq per direction. A retried object whose ack was lost is **re-acked but not
    delivered again**, or a lost ack turns one `VolumeUp` into two.
- **Key codes:** the integration's `KEY_CODES` is replaced by the real Elite map (Task 6). `key`
  **stays a u8 logical code**: `key_event` is `transport: both`, so widening it would break the
  WebSocket wire format the released v0.1.2 integration decodes, and evdev codes collide with
  `SOFT_KEY_BASE = 128` (128 is `KEY_STOP`). The remote maps evdev → logical (all < 128) from one
  generated table; only the HA event names change (stock names: `VolumeUp`, …).
- **IR data:** the CC0 [logitech-harmony-ir-archive](https://github.com/pickysysadmin/logitech-harmony-ir-archive)
  (276k devices, 684 protocol definitions, Pronto for every IR command) is the source for IR import
  later in Phase 4 and an independent oracle for `hub/ir/encode.c` tests now. Melodious and
  Afterglow were reviewed and are not used (direction confirmed by the user 2026-11-17).
- **Sleep:** the gate uses backlight dim/off only. MSP430 suspend (`/sys/power/state`) comes after
  the gate, tested on USB power with `standby` first — it is the riskiest remote write.

## Tasks

| # | Task | Depends | Device |
|---|---|---|---|
| 0 | Remote bring-up probes + `docs/hardware/remote-elite.md` | — | WRITE (remote only) |
| 1 | RF object codec: stock inner layout, rewrite `hub/rf/framing.*`, vectors | — | — |
| 2 | Hub `hub/io/rf_spi.c` driver + `hw_sinks.c` wiring + pairing helpers | 1 | — |
| 3 | Remote `remote/rf/` client (shared core, 0x20/32-byte mode, ≤32 B cap) | 1, 6 | — |
| 4 | Remote app: LVGL render of pages, evdev input, backlight, battery guard | 0, 6; 3 for wiring | — |
| 5 | `tools/deploy_remote.py`: push/run/status/install-hook/uninstall, fake-ssh tests | — | — |
| 6 | Real Elite key map in `proto/` + integration + tests; release note | 0 | — |
| 7 | `device_backup.py`: skip mounted UBI volumes | — | — |
| 7b | Configure our installed `ha_harmony` in HA against the running hub daemon | 2, 6 | WRITE (HA) |
| 8 | Hardware gate | all | WRITE (both) |

Order: {0-tooling, 1-framing, 5, 7} now → 0 on the device → 6 (schema, from 0's confirmed key
table) → {2, 3, 4} → 7b → 8. `rf_payload.c` adapts to the schema after 6, not in 1. Parallel tasks
touching `tests/c/CMakeLists.txt` merge by hand.

### Task 0 `[WRITE — remote only]` — bring-up probes

Also run `rf_probe --pipes` on the **remote**: it must hold hub address `A1B795EE` in slot 1. If
it does not, a remote-side bind becomes its own task before 3.

Small static ARM tool `tools/remote_probe.c` (allow-listed, like `rf_probe`): `fbinfo`
(VSCREENINFO/FSCREENINFO), `absinfo` (EVIOCGABS on event2), `keys N` (log event0/event2 for N s),
`name` (EVIOCGNAME on event0-3). Plus `rf_probe` cross-built for ARM with a hard 32-byte write cap,
run with `--version` (expect 72.00.0007) and `--pipes`. Stage binaries in `/var/volatile` (tmpfs).
Gate: pixel bitfields, ABS ranges, physical-key → code table confirmed by pressing each key, and the
remote's chip answering. Write `docs/hardware/remote-elite.md` from the recon plus these results.

### Task 1 — RF object codec

Rewrite `hub/rf/framing.{c,h}` to the inner layout above; keep `hp_rf_transport` unchanged.
Encoder/decoder, reassembly keyed by `(type, seq)`, CRC32, ack generation; `rf_payload.c` adapts.
Golden vectors in `tests/vectors/rf/` shared by hub and remote tests. Property tests: every object
1–255 B round-trips; any single corrupted, dropped or duplicated frame is detected and retried.

### Task 2 — hub driver

`hub/io/rf_spi.c` behind the `hp_rf_transport` vtable, with a syscall seam like `rfp_ops_t`:
hal preflight (fail closed), reader thread woken by `pthread_kill` + `SIGUSR1` (no `SA_RESTART`),
`write()==0` is success, link-status `10 <slot> 41 04 <status>` drives `attached()`,
`hp_rf_spi_set_pairing`/`hp_rf_spi_unpair`. On a negative `write()` (`-EIO`), back off and cap
the retry rate — the radio shares the flash SPI bus. Wire into `hw_sinks.c` (replacing the `-HP_E_NOSYS`
stubs) and the non-sim `main.c` path. Never touch `/dev/rffw`. Test the full contract of
`rf_pair.c` against the seam.

### Task 3 — remote RF client

`remote/rf/` on the same core: frames `0x20, seq|powermode, 0x12, slot, inner[28]`, write cap
asserted at 32, reader always running, power-mode byte from the doc (exact encoding confirmed in
Task 0 or left at stock's observed value). Sends `key_event`, receives `page_push`,
`tile_update`, `state_update`.

### Task 4 — remote app

`remote/main.c` replaces the stub: LVGL init, fb flush, page → widgets via `remote/page/page.c`,
button and tile widgets, evdev (event0 keys; event2 single-touch with Y > 320 strip handled by the
driver as KEY_HOME/KEY_FAVORITES), key LED and backlight via sysfs, idle dim → off, wake on input.
Fallback page ("Hub unreachable") **only when a send fails after all retries** — acks follow objects,
so an idle link is silent and must not trip it. Battery guard: poll STC3115 `soc` every 60 s and
`/sbin/poweroff` at ≤ 3 %, **only when the reading is fresh and valid and the power source reads
`battery` with no charge current**; a failed or stale read never powers off. Host tests: render each golden page into a memory fb,
compare hashes; input mapping; idle state machine; battery guard thresholds.

### Task 5 — `tools/deploy_remote.py`

Mirror `deploy_hub.py`: `status`, `push` (md5-verified into `/data/app`), `run` (foreground over
ssh), `install-hook` (the guarded line before `exit 0` in `/etc/init.d/rcS.local`, after dropbear,
with a boot counter that **disables the hook after 3 failed boots**, and a `/data/app/DISABLE`
kill switch; the app starts backgrounded after dropbear so it can never block SSH), `uninstall`.
**The hook is installed only after Task 8's 30-min idle test passes**; the gate itself uses `run`. Refuse anything touching
`/dev/mtd*`, `/dev/rffw`, `/dev/mspfw`, `/dev/watchdog`. Fake-ssh tests with a brick-safety command
log assertion.

### Task 6 — real key map

`proto/schema.yaml` and generated code: `key_event.code` becomes `u16` evdev; a single key table
(33 keypad + Home/Favorites + soft keys) generates C and Python. Integration `const.py`,
`event` entity types, `compiler.py` key references, docs, tests. Note the breaking change for
any automation keyed on the old provisional names in the release notes.

### Task 7 — backup safety

`device_backup.py`: read `/proc/mounts`, skip any mtd partition whose name matches a mounted
`ubi<N>:<name>` volume, record it in the manifest as `skipped: mounted gluebi volume (contained in
<ubi partition>)`, and make `--verify` treat that as OK. Test against the Elite's real layout.

### Task 8 `[WRITE]` — hardware gate

**No `device_backup.py` or any flash imaging while `rf_spi` runs** (shared SPI bus).
Quiesce hub; run hub with `rf_spi`; reboot the Elite (clears the D-state readers), start our app;
HA (our component, installed for this) pushes a one-page config; tap a button, press VolumeUp;
observe IR on the TV and both events in HA; 30 min idle on the cradle. Restore hub.

## Open questions

1. ~~HA install~~ — answered: `ha_harmony` is already installed via HACS with no config entry; it
   is configured in Task 7b. The stock `harmony` integration stays until the user removes it.
2. Pairing: both chips hold the slot-1 pairing today. If our frames are accepted under it, no re-pair
   is needed; if not, Task 2's `set_pairing` and a remote-side bind are required.

## Bench session 1 (2026-11-17, 07:10–07:30 UTC) — gate partly met

Hub quiesced at 07:10:02; our daemon `hub --token … --rf-spi --rf-slot 2`, the Elite running
`harmony_remote --slot 1 --powermode 0x30`; `ha_harmony` v0.2.0 (released for this) configured in
HA against the hub. Hub restored to stock afterwards.

**Met — remote → hub → IR → HA, on real hardware:**
- Hard **Vol +** and **Mute** on the Elite → our app → RF → our hub daemon → key mapping → **real IR;
  the Samsung TV obeyed both** (observed by the user). HA's event entity reported `volume_up`
  (logical key 8, remote 1); an earlier unprompted press arrived as `play` (key 14).
- HA compiled the gate config and pushed it; the hub wrote `cache.json` (35 KB).

**Not met — hub → remote:** the 2-frame `page_push` never reached the remote (it logged nothing,
not even a rejected report), so the Elite stayed on the app's boot page ("Harmony"). Single-frame
hub→remote objects (acks) most likely do arrive: the remote never fell back to "Hub unreachable"
after its heartbeats. The hub's radio emits `20 02 42 00` / `20 02 42 01` (TX status, slot 2) after
transmissions, which the driver dropped; the leading hypothesis is that the second frame of an
object is written before the radio has finished the first. Next: per-frame pacing on TX status or a
minimum gap, and `--rf-debug` frame logs with timestamps on both ends. The 30-min idle soak was not
run.

**Corrections found at the bench:**
- **Slots are HID++ device indices, which start at 1.** The Elite is device **2** on the hub
  (`10 02 41 04 87 21 40` = link up, wpid 0x4021), and the hub is device **1** on the remote
  (`10 01 41 04 07 0b 88`, wpid 0x880B). The pipe table had been read off by one (pipe `0x20` is
  device 1). Addressing the wrong index gives `10 01 8f 40 0d 09 00` (error 0x09) per frame.
  `--rf-slot` (hub) and `--slot` (remote) set them; learning them from link status is a follow-up.
- `deploy_hub.py run` forces a TTY (`ssh -tt`) and fails from a non-interactive session; the
  daemon was started detached on the device instead. Needs a `--detach` mode.
- A `ps | grep` stop-check matched its own ssh command line and reported the app as still running;
  there is no evidence the app is slow to stop. Use `pidof` or match the binary path with `awk`.
- The released v0.1.2 integration rejected the new key names, so v0.2.0 was published from this
  branch (tag on `phase4-rf-remote`, not `main`).
