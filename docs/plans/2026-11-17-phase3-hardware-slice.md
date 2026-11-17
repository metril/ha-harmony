# Phase 3: First E2E Slice on Real Hub Hardware

Spec: `docs/design/2026-11-16-ha-harmony-design.md` (Phase 3 + "Corrections from the real rooted hub").
Conventions: `docs/plans/2026-11-16-phase2-foundations.md`. Hub state: `docs/runbooks/phase1-hub.md`.

**Goal:** one closed loop on the real hub — HA pushes a config built from the extracted stock IR
database → a simulated button press arrives over the hub's RF socket → the hub emits **real IR** at a
real device in the room → the press surfaces in HA as an event.

**Scope decision (user, 2026-11-17):** the physical Elite remote is **deliberately not rooted** —
deferred until we have firmware to deploy, so the living room keeps working. This slice is
**hub-only**: the "button press" comes from `remote_sim` over the hub's Unix RF socket, not over RF.
The CC2544 radio is not used at all in this phase.

Every task is `[RO]` (read-only on the hub) or `[WRITE]`. **A `[WRITE]` task starts only on the
user's go-ahead at the moment it runs** — Tasks 5 and 6 stop the stock stack and therefore
temporarily break the user's working remote *and* HA's stock `harmony` integration (7 automations).

## Global constraints

- Only `/data` and `/cache` may be written. No partition is ever flashed. `/` is unionfs
  (`/mnt/data` rw over `/mnt/root` ro squashfs).
- **Nothing is armed at boot.** No init script, no boot gate, no cron, no respawn. Our daemon runs
  only in a foreground SSH session. **A reboot returns the hub to full stock** — the primary rollback
  for every `[WRITE]` step here, and it must stay true by construction (asserted by a test over the
  emitted command log, like Phase 1's brick-safety test).
- **Contention:** stock `/usr/bin/hal -f -s` holds `/dev/i2s` write-only and `/dev/rfspi` rw for its
  lifetime; `/usr/bin/lua main.lua` is the app layer. Our daemon cannot emit IR until both stop.
  Quiesce windows are short, announced, and always ended by restoring stock.
- Device BusyBox lacks `dd`, `tar`, `sha256sum`, `find`, `head`, `df`, `sync`, `sort`, `readlink`,
  `setsid`, `nohup`, and the `command` builtin. Stage `payload/bin/busybox` (MIPS static 1.36.1) into
  `/tmp` when those are needed; `/tmp` is tmpfs, so it vanishes on reboot — a feature here.
- IR timings and carrier come from the hub's own `ProtocolList` values as captured in
  `backups/phase1/hub/<ts>/extracted/ir_codes.json` — **never** textbook constants.

---

### Task 0 `[RO]` — Float ABI, CPU target, and MIPS tests in CI  ← **the blocker**

Found by running our cross-built C tests on the real hub, 2026-11-17.

**The defect:** we build hard-float (`readelf -A`: `ISA MIPS32r2`, `FP ABI: Hard float`, flags
`0x70001007`). The hub is an **AR9330 (Hornet), MIPS 24Kc V7.4, no FPU**, on a 2.6.31 kernel with no
FP emulation — so every FP instruction is SIGILL. Stock `/usr/bin/hal` is `mips32`, **`Soft float`**,
flags `0x50001007`. Our toolchain file also targets the wrong core (`-mtune=74kc`, i.e. AR9344).

On-hub results before the fix: SIGILL (132) in `test_jsonrpc`, `test_config2`, `test_mapping`,
`test_activity`, `test_irout`; SIGSEGV (139) in `test_ws`; `test_page` rc 1; `test_ir_encode` rc 2.
Passing: `test_framing`, `test_rf_payload`, `test_sched`, `test_mdns`, `test_rf_pair` — exactly the
ones that execute no floating point. The daemon also ran but closed every WS handshake with 1008,
while the same code built for the host handshakes fine.

**Why CI missed it:** cross-builds default `HP_BUILD_TESTS` off, so we build MIPS binaries and never
execute one. That is the real hole to close.

- [x] Soft-float toolchain chosen: **OpenWrt 24.10.0 ath79 SDK** (gcc 13.3, musl, `mips_24kc`, soft
      float). Bootlin publishes no soft-float big-endian mips32 toolchain. Provisioned by
      `tools/fetch_toolchains.sh`, pinned by URL + sha256, installed to `vendor/toolchains/`
      (gitignored).
- [ ] `cmake/toolchains/hub-mips.cmake`: `-march=mips32r2 -mtune=24kc` (r2 is right — the 24Kc *is*
      MIPS32 Release 2; the float ABI was the bug) plus soft-float flags. Gate: `readelf -A` on the
      hub binary reports **`FP ABI: Soft float`**.
- [ ] `.github/workflows/ci.yml`: same pinned toolchain as the script (a test asserts they cannot
      drift), and **run the MIPS C tests in CI** — `-DHP_BUILD_TESTS=ON` with `qemu-mips`
      (`qemu-user-static`) or ctest's `CROSSCOMPILING_EMULATOR`. A big-endian soft-float test run in
      CI is the deliverable; without it this class of bug returns.
- [ ] Re-run the whole C suite **on the hub**; every test that passes on the host must pass there.
- [ ] Any test still failing after the ABI fix is a genuine big-endian/portability bug — fix it with a
      host regression test, never by disabling the test. `test_ws`'s SIGSEGV and the 1008 handshake are
      the prime suspects; they may evaporate with soft float or may be real BE bugs.
- [ ] Gate: on-hub suite green, and the integration's own client completes a handshake against the
      daemon running on the hub.
- [ ] Rollback: host-side only; nothing on the hub beyond `/tmp`.

### Task 1 `[RO]` — Reproducible cross-build of `hub` + `remote_sim`

`remote_sim` is the press generator and the hub's RF socket is a Unix socket **on the hub**, so
`remote_sim` must run there. It links only `hp_hub` and plain sockets, so cross-building it is a guard
change, not a port. (Rejected alternative: an SSH-forwarded Unix-socket bridge — the hub has no
`socat`, and cross-building keeps the press path identical to the host E2E.)

- [ ] `CMakeLists.txt:68`: build `remote_sim` when `NOT CMAKE_CROSSCOMPILING` **or** the target is the
      hub, behind an option defaulting ON for both.
- [ ] Gate: `file` reports `ELF 32-bit MSB … MIPS … statically linked` for both; `readelf -d` shows no
      dynamic entries; both run on the hub. Record `/data` free space from the Phase 1 image before
      planning a push (it is a 5 MB jffs2).

### Task 2 `[RO]` — Real IR sink behind `hp_ir_sink`

**`docs/hardware/hub-ir-i2s.md` exists** (`eac5838`), from static analysis of the stock `ath_i2s.ko`
(not stripped) and `hal` (keeps ~486 dynamic symbols incl. `ir_send`, `ir_init`, `bitbuf_*`, and
debug strings naming `src/libhal/ir/libir.c`). Nothing in it is hardware-verified yet. Summary:

- `/dev/i2s`, major 253, opened `O_WRONLY`; the speaker slot is exclusive, so a second write-open
  gets `-EBUSY` ("I2S speaker busy") — hence the quiesce requirement. Read opens are a separate slot.
- Driver implements `read`, `write`, legacy `ioctl`, `open`, `release` — no `mmap`, no `poll`. Every
  ioctl arg is a plain integer by value. Numbers agree between module and `hal`: `0x80044e20` volume
  (15), `0x80044e21` `I2S_FREQ` (16.16 fixed-point bit-clock divider → register `0xb80b001c`),
  `0x80044e22` data size (16), `0x80044e23` mode (2), `0x80044e30` start `(ports<<16)|0` (begins DMA),
  `0x80044e31` drain (blocks until queued audio has played).
- **IR is a bitstream, not pulse timings:** one bit per I2S bit-clock with the carrier already baked
  in; mark = runs of `1`, space = `0`, packed MSB-first, written as 16-bit words with adjacent byte
  pairs swapped. Carrier built from N bits/cycle (N ∈ 2,4,8,10,12 by carrier period); bit time
  `ceil(X/N)` ns; mark/space quantised to whole carrier periods. `write()` fills a ring of 512×768 B
  (384 KiB) and blocks when full; it does **not** start the DMA — the start ioctl does.
- Sequence: open → data size, mode, volume → clock → `write()` bitstream → start → drain.

**Unverified, and what the bench must settle:** the physical bit time for a given divider; the
byte-swap direction and mark polarity; the meaning of the `ports` GPIO bits (which bit drives which
emitter — ties to open question 4); the mode bit; idle level and underrun behaviour; and whether
`hal` really holds the node for its lifetime or reopens per send (the code suggests per-send, which
conflicts with the observed persistent fd).

> **RETRACTED 2026-11-17.** The "proven" claim below rested on a single TV state change at
> 19:29:59, and that evidence does not hold. In session 2 the same tool and arguments produced no
> effect across ~10 emissions, including one fired seconds after a *successful stock* send had
> primed the hardware, and with the user watching the screen directly. The TV is also known to
> change state unprompted — it went `off` 19:34:26 → `on` 19:34:45 while the hub was mid-reboot with
> nothing emitting. So 19:29:59 was most likely coincidence.
> **Nothing below is confirmed: not `ports` bit 17, not the byte-swap direction, not the mark
> polarity, not the bit time.** `hub/io/hw_sinks.c` was built against these values and is therefore
> also unverified on hardware (its host tests only prove it matches `ir_bench` byte-for-byte, which
> is agreement, not correctness). What IS established: every ioctl returns 0, `DRAIN` blocks for the
> full predicted waveform (so the DMA consumes the bitstream), and **stock firmware drives the same
> emitter successfully** via `remote.send_command`.
> **Next step must be an oracle independent of the TV** — see "Session 3 plan" below.

- [x] ~~**DONE — real IR emitted and proven, 2026-11-16 19:29 EST (`fffb1cd`).**~~ *(retracted)* `tools/ir_bench.c`
      (separate CMake target, never linked into `hub`) emitted a Samsung `PowerToggle` from the hub's
      built-in emitter and **`media_player.un55js9000` went `off` → `on`** — an automated oracle, no
      human watching needed. Succeeded on attempt 5 of a capped ladder, one variable per attempt.
      **Confirmed working parameters:** carrier 37900 Hz → period 26385 ns, **N=12**, duty 33 %
      (4 one-bits then 8 zero-bits per cycle), bit time **2199 ns**, `FREQ` divider **`0x0036f999`**
      (54.975 = bit_ns/40), byte-pair **swap on**, mark **not inverted**, **`ports=2`** →
      `START` arg `0x20000`, i.e. **bit 17 = the built-in emitter** (bits 16/18 are GPIO16/GPIO13 and
      emitted nothing). Stream: 147,392 bits / 18,424 bytes / 324.1 ms — that is **three repeats**
      (`min_repeats: 3`); **one** frame is ~108 ms / 49,152 bits / 6,144 bytes, so do not read 324 ms
      as a per-frame figure. A single `write()` took 0.2 ms
      and `DRAIN` blocked 323.5 ms against a predicted 324.1 ms — so the divider formula yields the
      intended bit time. Sequence exactly as documented: open `O_WRONLY` → `DSIZE` 16 → `MODE` 2 →
      `VOLUME` 15 → `FREQ` → `write` → `START` → `DRAIN` → close, every call returning 0.
- [x] **Two hazards found, both now load-bearing for Task 4:**
      1. **Never open `/dev/i2s` while stock `hal` is running — not even read-only.** A read-only
         open followed by `close()` **hung in the kernel in `ath_i2s_close`**, spinning in state R and
         **immune to `kill -9`**. `tools/ir_bench --probe` must only be used after quiescing.
      2. **Restarting `hal` by hand does not work.** Re-running `/usr/bin/hal -f -s` after the kill made
         it die immediately leaving zombie `[hal]` children, and luaworks would not stay up, so
         `remote.harmony` stayed `unavailable`. **A reboot is the reliable restore** (~4–5 min total
         outage measured, including reboot) and it is safe precisely because nothing is armed at boot.
         Task 4's `restore` should therefore **reboot by default** rather than attempt a hand restart.
- [ ] Remaining unknowns: what `ports` bits 16/18 actually drive and whether bit 17 only selects or also
      enables; whether swap and polarity matter independently (attempts 2–3 used `ports=0`, which
      emitted nothing, so they do not independently rule out the alternatives); the `I2S_MODE` bit; idle
      level and underrun behaviour; the root cause of the close-hang; and whether codes at other
      carriers (other N values) work — the next bench round should sweep one code per distinct carrier.

- [ ] `hub/io/hw_sinks.{c,h}`: `hp_hw_sinks_open()` returning `hp_sinks_t`. The IR sink translates
      `hp_ir_signal_t` (alternating mark/space, mark first) into that wire format. **Injectable fd and
      writer function pointer** so host tests cover the translation with no device. BT and RF-radio
      sinks return `-HP_E_NOSYS` this phase.
- [ ] Host-tested: golden byte vectors for Samsung `PowerToggle` and an Audioengine command taken from
      the real `ir_codes.json`; the carrier/ioctl call sequence asserted; short/partial/`EINTR` writes
      retried; oversize signal rejected, never truncated; fd closed on every error path. Under
      ASan+UBSan in `ctest`.
- [ ] **If the interface is partly unknown, do not guess in C.** Bench ladder on the hub, inside a
      Task 4 quiesce window: write one candidate encoding to `/dev/i2s` and use layered oracles —
      (a) phone camera sees the emitter flicker → framing/ioctl roughly right; (b) the target device
      reacts → timing and carrier right; (c) neither → `strace` stock `hal` through one known command
      and match the syscall sequence. Record every candidate and result in the hardware doc. The C
      sink gets written once (a) and (b) pass.

### Task 3 `[RO]` — `hub/main.c` real (non-`--sim`) path

`hub/main.c:101` currently prints `no hardware backend built` and exits 1 without `--sim`; hub_id and
fw_version are literals at `:153-154`.

- [ ] Without `--sim`: wire `hp_hw_sinks_open()`; default `--cache /data/ha-harmony/cache.json`,
      `--state /data/ha-harmony/state.json` (atomic via `hub/engine/atomic_file.c`), and `--rf
      /tmp/ha-harmony-rf.sock` so the socket never persists.
- [ ] Real identity: stable `hub_id` (the stock hub id (recorded locally) keeps HA's unique_id recognisable —
      see open question 5) and a real `fw_version` from our build plus the stock FW, not a literal.
- [ ] Fail loudly and specifically when `/dev/i2s` is busy: "stock hal still holds /dev/i2s — run
      tools/deploy_hub.py quiesce first". A vague error here is the likeliest bench time-waster.
- [ ] Host-tested with an injected fake hw-sink factory: default paths, identity, busy-device text.

### Task 4 `[WRITE — go-ahead needed at the moment it runs]` — Deploy tooling and the quiesce window

The step that temporarily breaks the living room: the physical remote stops working and HA's stock
`harmony` integration goes unavailable for the length of the window.

- [ ] `[RO]` **Discovery first:** record how `hal` and `lua main.lua` are started and whether anything
      respawns them (`/etc/init.d/*`, `/etc/inittab`, any supervisor). Restore must use the same
      mechanism in reverse. If a supervisor respawns `hal`, prefer stopping the supervisor; if that is
      not clean, **stop and report** — do not kill-fight a supervisor on the user's only hub.
- [ ] `tools/deploy_hub.py` over the `harmony-hub` SSH alias, subcommands: `push` `[WRITE]` (create
      `/data/ha-harmony/`, copy `hub`, `remote_sim`, config, busybox; verify each with `md5sum` both
      ends, then `sha256sum` once the staged busybox is there; refuse below a free-space margin);
      `quiesce` `[WRITE]` (stop `lua`/`luaworks`/`luadraws` then `hal`, confirm `/dev/i2s` and
      `/dev/rfspi` are unheld, print a loud banner + timer); `run` `[WRITE]` (**foreground over SSH**,
      streaming output — never `nohup`/`&`/`setsid`, none of which the device even has); `press KEY`
      `[WRITE]`; `restore` `[WRITE]` — **reboots the hub by default**, because a hand restart of `hal`
      was measured not to work (see Task 2); `clean` `[WRITE]`.
- [ ] `quiesce` must refuse to run anything that opens `/dev/i2s` until it has confirmed no process
      holds it — a read-only open against a live `hal` wedges unkillably in `ath_i2s_close`.
- [ ] Safety asserted by a command-log test: nothing writes `/etc/init.d`, `/etc/inittab`,
      `/etc/tdeenable`, `/etc/rc*`, crontabs or any `/dev/mtd*`; no backgrounding; every written path is
      under `/data/ha-harmony` or `/tmp`.
- [ ] Gate: dry-run against the Phase 1 fake SSH transport; on-device `push` + `hub --help`; then
      `quiesce` → `restore` round-trips with the user confirming the physical remote **and** HA both
      work again, **before** any IR is attempted.
- [ ] Rollback: `restore`, then `clean`. If restore fails: **reboot** (nothing is armed at boot, so it
      comes up stock). If that fails: hardware factory reset, then re-root per the Phase 1 runbook.

### Task 4b — Repeat fidelity: the config schema cannot express how stock actually sends IR

Found while building the converter (2026-11-17). **Decide before the E2E slice is judged.**

The schema's `raw: {hz, us}` is a single mark/space sequence; `hub/engine/irout.c`'s `encode_raw`
sets `rep_n = 0`, so the hub sends it once per press and re-sends the whole sequence while held. But
the stock hub sends **every one of the 85 commands with `min_repeats` ≥ 1** (3 for the Samsung
protocol), and the 18 NEC/Toshiba-style commands use a **separate short repeat frame**
(8990/2230/568/96077) rather than resending the full frame. Converted output therefore carries
`min_repeats_not_expressed` on all 85 and `repeat_frame_not_expressible` on 18.
`pronto` is not an escape: it quantises durations to carrier periods and derives the carrier from a
frequency word, so 37900 Hz and 568/552 µs would not survive.

Options:
- `--repeats expand` (exists, works): pre-multiplies `once + repeat × min_repeats + finish` inline.
  Costs **15,108 of the hub's 16,384 pool words** for this device set alone — almost the whole budget,
  leaving no room for pages or more devices — and must clamp the 96077 µs inter-frame gap to 65535
  (u16 storage).
- **Extend the config/proto with `min_repeats` and an optional repeat frame** (preferred): a small
  change to a format we own, keeps the pool cost at 5,780 words, and matches what the hardware does.
- Ship single-frame and see which devices tolerate it. Cheapest, but some IR receivers require the
  repeats, so this may be what decides whether real presses feel reliable.

- [ ] Pick one. If the slice's IR works but feels flaky, this is the first suspect.

### Task 5 `[WRITE — go-ahead needed]` — The E2E slice

- [ ] `[RO]` `tools/stock_to_ha_config.py`: `ir_codes.json` → an `ha_harmony` config using the existing
      `raw: {hz, us}` action form, already accepted end-to-end by `config_model.py:120-126` and
      `hub/engine/mapping.c:127-157` — so no new wire format, just a converter. Covers the 85 real IR
      commands; NVIDIA HID and Roku ECP entries are skipped **with an explicit warning each**, never
      silently. Tested against the real extracted file.
- [ ] `[WRITE]` HA config entry against the real hub (`192.0.2.11:8099`, manual host/port/token).
      Runs **alongside** the stock `harmony` entry — do not disturb it, it is the user's fallback.
- [ ] `[WRITE]` Push the config; expect `config_ack` and the cache to appear at
      `/data/ha-harmony/cache.json`.
- [ ] `[WRITE]` **First real IR: `send_ir` to the J-Tech AV switch input select** — the safest visible
      target: non-destructive, idempotent, visibly confirmable on the TV, does not power-cycle the TV or
      audio, and 9/9 of its commands were extracted. Fallback if it does not respond: Samsung
      `PowerToggle` (`0xE0E040BF`, GoVideoO1 32 Bit, 37900 Hz) — unmistakable, but it toggles the user's
      TV, so only with them watching.
- [ ] `[WRITE]` Then the full loop: `deploy_hub.py press <KEY>` for a key mapped to that action → real
      IR → HA event entity fires. **Slice gate:** the target device reacts *and* the HA event appears,
      from one simulated press, with no stock stack running.
- [ ] `[WRITE]` End the window: `restore`; confirm stock remote + stock HA integration.
- [ ] Record it all in `docs/runbooks/phase3-hub-slice.md`.

### Task 5b — Bench session 2 (2026-11-16 20:00–20:30 EST): slice runs, IR does not land

**What was proven.** `deploy_hub push` (md5-verified both ends) → `quiesce` → the daemon running on
the real hub in its **non-sim hardware path** (`hub_id=ha-harmony-pimentoblue`,
`fw=ha-harmony-hub`) → **HA's own `HarmonyHubClient` connected to it** → `ir_send` with the real
extracted Samsung `PowerToggle` returned `{'ok': True}` → `remote_sim --press 2` **on the hub** came
back to that client as `key_event {remote: 1, key: 2, action: 0}`. Every link in the loop works
except the last hop.

**What failed.** No IR reached the TV — not from our sink, and not from `ir_bench` with the exact
parameters and syscall trace that worked at 19:29:59. Five+ sends, every ioctl returning 0 and
`DRAIN` blocking the full 323.5 ms, so the DMA consumed the whole bitstream each time.

**Then the stock firmware worked.** `remote.send_command` (device `Samsung TV`, `PowerToggle`,
3 repeats) through HA's stock integration — untouched Logitech code, same hub, same emitter — **did
turn the TV on**. So the emitter has line of sight from the hub's new position, and the fault is
**ours**, not aim.

**Two methodology lessons, both of which inverted a conclusion mid-session:**
1. **`media_player.un55js9000` lags.** It still read `off` *after* the stock command had visibly
   turned the TV on. It was reliable at 19:29 and unreliable here, so it is **not a trustworthy
   oracle on its own** — confirm with the user's eyes, or poll for 30 s+ before concluding anything.
2. A **physical change invalidated the baseline**: the hub was moved to a longer USB cable, which
   also power-cycled it (fresh boot, `/tmp` wiped, stock stack back, `/data/ha-harmony` intact) and
   **swapped the hidraw numbering** — `hidraw3` is now the remote (`c12b`), `hidraw4` the hub
   (`c129`). Our udev rule matches on PID, so access was unaffected. Also note the physical remote
   has **its own IR blaster**, so pressing it is *not* a test of the hub's emitter.

**Leading hypotheses for why ours does not land, cheapest first:**
- **(a) Marginal modulation.** Our carrier duty/N/divider may be close-but-wrong, producing a weak
  or mis-shaped carrier that decoded once at the old (closer) position and never at the new one.
  Stock IR is robust at both. **Test:** parameter sweep at the new position — duty 25/33/50, N
  8/10/12, `--swap 0/1`, `--invert 0/1`, `--ports 2/6` — with the user watching as the oracle.
- **(b) Missing initialisation that stock `hal` does.** Killing `hal` may leave a GPIO
  direction/output-enable or clock mux unset that it configures at startup, so our emission is
  correctly timed but never reaches the LED. **Test:** have stock send IR (priming the hardware),
  then quiesce *immediately* and run `ir_bench` with the 19:29 parameters. If it works, this is it.
- **(c) The 19:29 success was conditioned on prior attempts.** Attempts 1–4 that night poked
  `ports=0/1` (GPIO16) before attempt 5 succeeded on `ports=2`. **Test:** replay the 1→5 sequence.
- **(d) An IR receive path exists** (`/dev/i2s` implements `read`, and the mic slot is separate from
  the speaker slot), so the hub may be able to **capture its own stock emission and ours and diff
  them** — the strongest possible evidence, and worth the effort if (a)–(c) do not settle it.

### Session 3 plan — get an oracle that does not depend on the TV

The TV is a bad oracle: HA's `media_player` state lags by minutes, and the set itself wakes on its
own (CEC from the Shield, or a network wake). Two sessions were spent partly chasing that. Fix the
oracle before touching parameters again.

- [ ] **(1) Phone camera on the hub's emitter — 20 seconds, splits the problem in half.** CMOS
      sensors see 940 nm as a pale violet flicker. Sweep `--ports 1..7` one value at a time while the
      camera watches. **Any flicker** → the LED is driven and the fault is framing/timing, and we have
      found the real emitter bit. **No flicker on any mask** → nothing reaches the LED, so `ports`
      bit 17 is wrong and the START arg's meaning needs rethinking. Everything else is guesswork until
      this is known.
- [ ] **(2) Capture ground truth from stock.** `/dev/i2s` implements `read` and the mic slot is
      separate from the speaker slot, so the hub may be able to record while stock emits (triggered
      over HA, no quiesce needed) and again while we emit, then diff the two. This replaces inference
      with the actual waveform stock puts on the wire. Highest value if (1) is ambiguous.
- [ ] **(3) Failing that, trace stock's own syscalls and bytes** and replay them verbatim: if stock's
      exact byte stream works from our process, our framing is wrong; if it does not, our call
      sequence or device state is wrong. The device has no `strace`, so this needs a staged
      `strace` build or a `ptrace` helper — more work, but it is conclusive.
- [ ] Also fix `ir_bench --duty`: it is a **no-op**. Setting 25 or 50 produced a byte-identical
      stream to the default (the N=12 pattern hardcodes 4 ones / 8 zeros), so "duty swept" in session
      2 tested nothing. Either implement it or remove the flag.

### Task 6 `[RO]` — Verification ladder: did IR actually fire?

- [ ] Oracles in order: (1) daemon log says the sink wrote N bytes without error; (2) phone camera sees
      the emitter flicker (CMOS sensors see 940 nm); (3) **the target device reacts** — the real gate;
      (4) the user confirms what they saw.
- [ ] Success reported but no LED → wrong fd/ioctl, or stock `hal` still holds the device. Re-check
      holders, then back to Task 2's bench ladder.
- [ ] LED fires but device ignores it → timing/carrier/aim. Confirm values came from `ProtocolList`
      (37900 Hz; 8990/4490/568/552/1662 — **not** 38000 and textbook NEC), check repeat count and
      inter-frame gap in `hub/engine/irout.c`, then aim, then an IR receiver as an independent decoder.
- [ ] Nothing after two bench rounds → stop, restore stock, write up findings. Do not keep the living
      room down to chase it.

---

## Rollback summary

| `[WRITE]` step | Rollback |
|---|---|
| `push` (files under `/data/ha-harmony`) | `clean`; or reboot then `clean`; factory reset wipes it regardless |
| `quiesce` (stock stack stopped) | `restore`; else **reboot = full stock** |
| `run` (our daemon) | Ctrl-C / SSH drop; nothing armed at boot |
| HA config entry | delete the entry; stock `harmony` entry untouched throughout |
| bench IR experiment | nothing persisted; ends with `restore` |

**UART note:** still not a prerequisite, for the same reason as Phase 1 — every write lands in the
factory-reset-wiped area, nothing is flashed, and this slice deliberately arms nothing at boot, so a
reboot is always an escape hatch. It becomes a real prerequisite the moment we add a boot-time hook
(Phase 4 persistence): a bad one can boot-loop past the reset.

## Open questions for the user

1. **Quiesce window** — how long a living-room outage is acceptable per bench session, and when
   (daytime vs evening TV time)? Sets whether Tasks 4 and 5 run in one window or two.
2. **IR target** — confirm the J-Tech AV switch input select is safe to toggle, and whether the Samsung
   `PowerToggle` fallback is acceptable with them watching.
3. **mDNS** — acceptable to advertise `_haharmony._tcp.local.` from the hub while the stock stack is on
   the LAN, or run this slice `--no-mdns` with a manual HA entry?
4. **IR port mapping** — the hub has a built-in emitter plus wired blaster ports. Does the living-room
   setup use blasters, and which port faces which device? `port_mask` needs a real mapping.
5. **Hub id** — reuse the stock hub id (keeps HA unique_ids stable and recognisable) or mint our own?
6. **Second backup copy** — still open from Phase 1. Phase 3 is the first phase that writes to hub
   flash, so decide before Task 4 runs.

## Testable without hardware

Toolchain pin agreement with CI; the cross-build artifact shape (`file`/`readelf`, including the
soft-float assertion); all IR-sink byte translation with an injected writer (golden vectors from the
real extracted codes, short/partial/`EINTR` writes, oversize rejection, fd cleanup); the non-`--sim`
startup path with a fake hw-sink factory; `deploy_hub.py` fully against the fake SSH transport
including the no-persistence/no-mtd-write assertion; `stock_to_ha_config.py` against the real
`ir_codes.json` (85 converted, 48 skipped with warnings, 0 silent drops). Baseline to keep green: 464
Python tests, host `ctest`, `ruff check .`, **and now the MIPS C suite under qemu in CI**.
