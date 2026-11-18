# Phase 4: Boot persistence

Spec: `docs/design/2026-11-16-ha-harmony-design.md`. Hub state: `docs/runbooks/phase1-hub.md`.
Hardware: `docs/hardware/hub-ir-i2s.md`. Previous phase: `docs/plans/2026-11-17-phase3-hardware-slice.md`.

**Goal:** our firmware runs the hub after a power cycle, with no host attached, and stock remains a
one-file-removal fallback.

**What changes from every previous phase:** until now a reboot was an unconditional escape hatch,
because nothing was armed at boot. Phase 4 deliberately gives that up. **This is the first phase that
can lock us out of the device**, so most of the plan below is about keeping a way back in rather than
about running our daemon.

**User decision (2026-11-17):** a second off-host copy of the Phase 1 images is **waived**. Recorded
because it changes the risk calculus below: the only images of this hub live on one host, so recovery
from a bricked flash means a factory reset and re-root, not a restore. That is acceptable precisely
because this plan writes nothing to flash (see below) — if the design ever starts programming mtd,
revisit this.

---

## 1. Verified boot path

All read-only from the live hub, 2026-11-17. Line numbers are in `/etc/init.d/rcS`.

| step | what happens | evidence |
|---|---|---|
| `inittab` | `::sysinit:/etc/init.d/rcS` — the only entry besides `ctrlaltdel` | `/etc/inittab` |
| `rcS:4-36` | `mount -a`, hostname, `/var/volatile` dirs, syslogd/klogd, `insmod` each line of `/etc/modules` | `rcS` |
| `rcS:42-43` | **`source /etc/init.d/rcS.local`** if executable — "product specific configuration" | `rcS:42` |
| `rcS:47-53` | **dropbear**, gated on `[ -r /etc/tdeenable -a -x /usr/sbin/dropbear ]` | `rcS:47` |
| `rcS:56` / `rcS:59` | `LD_PRELOAD=/lib/libcrashlog.so`; watchdog unless `/etc/nowatchdog` | `rcS:56,59` |
| `rcS:84` | **hal**, gated on `[ "$DEMO_MODE" != 1 -a -x /usr/bin/hal ]` | `rcS:84` |
| `rcS:~90` | `ifup -a &`, then `usbhid -s &` | `rcS` |
| `rcS:97`, `rcS:102` | `luaworks` then `luadraws`, each gated **only** on the binary being executable | `rcS:97,102` |

**`rcS.local` is stock and load-bearing.** It is not a free hook: it reads the hardware version from
the `mfg` partition, picks a CSR8811 pskey file from the Bluetooth build name, loads it with `bccmd`,
and runs `hciattach ttyS0 bcsp`. It also forces `vm.overcommit_memory=1` with the comment *"here be
dragons .. as Pimento has limited RAM ... if we actually run out of memory the OOM killer is let lose,
and it will result in watchdog reboot"*. Clobbering this file costs Bluetooth and the memory policy.

**Writes persist with no flash programming.** `/` is `unionfs (dirs=/mnt/data=rw:/mnt/root=ro)`, with
`/mnt/data` = mtd4 jffs2 (5 MB, also holds `/data`) and `/mnt/root` = mtd3 squashfs (4 MB, read-only).
So a write to `/etc/...` copies up into mtd4: it survives reboot and is **wiped by a factory reset**.
That is the whole persistence mechanism — no `mtd` writes, no u-boot changes, no kernel flashing.

**We already depend on this, successfully.** Our SSH access persists by exactly this route: the root
tool created the marker `/etc/tdeenable` (0 bytes) and a 55-byte wrapper `/usr/sbin/dropbear`
(`exec /data/dropbearmulti dropbear -s -g "$@"`), both in the rw layer, and stock `rcS:47` then starts
it every boot. Phase 4 is the same trick applied to our daemon.

**`DEMO_MODE=1` skips `hal`.** It is referenced only at `rcS:84` and defined nowhere, so exporting it
from `rcS.local` suppresses `hal` without editing the line that starts it. This is how we free
`/dev/i2s` and `/dev/rfspi` at boot instead of killing `hal` afterwards.

**Stock already demonstrates both patterns we need.** The `/etc/memtester` block in `rcS.local` is a
marker-gated alternate boot mode, and it guards itself: if `/cache/memtester.run` exists the previous
run crashed, so it lights the LED red and **stops instead of retrying**. Copy this shape.

**Other facts that bear on the design**
- **Watchdog is currently OFF.** `/usr/bin/watchdog` runs unless `/etc/nowatchdog` exists, and the
  root tool created that file. So today a wedged hub stays wedged; there is no automatic reboot.
- **RAM is not a constraint.** 61 MB total, **34 MB free** with the entire stock stack running. Our
  daemon is a 702 KB static binary. (Stock's own comment still warns RAM is tight — measure RSS.)
- **BusyBox is v1.13.4 (2017)** and lacks `dd`, `sha256sum`, `tar`, `find`, `head`, `wc`, `df`, `sync`,
  `timeout`, `setsid`, `nohup`, `grep -E`, and the `command` builtin. `grep -r` does work, but is slow
  enough over `/etc` to time out an SSH command. Any boot script must stay inside this floor — and note
  the two absences that bite a boot script hardest: **no `timeout`**, so there is nothing to bound a
  blocking call with, and **no `nohup`/`setsid`**, so detaching means plain `&` and redirection.
- Partitions: `u-boot` 64 K, `kernel` 960 K, `kernel2` 960 K, `root` 4 M, `data` 5 M, `cache` 5 M, `mfg` 64 K.
  **`kernel2`'s role is unverified** — it may be an A/B slot or an OTA staging area. Phase 4 does not
  need to know, because it touches no kernel; do not build a design that depends on a guess here.

## 2. The actual risk, stated plainly

`rcS.local` is sourced at `rcS:43`, and dropbear does not start until `rcS:47`. **Anything we hook
into `rcS.local` runs before there is any way to log in.** A hook that hangs, blocks on the network,
or crashes the shell leaves a hub with no SSH, no stock stack and no remote recovery — only UART or a
factory reset. Three consequences, which drive every decision in §3:

1. Our hook must **start dropbear itself, first, before doing anything else.**
2. It must be **incapable of blocking** — everything else backgrounded, no unbounded waits, and note
   there is no `timeout` applet to lean on.
3. It must **disable itself after repeated unclean boots**, so a crash-loop converges on stock rather
   than on a brick.

The UART adapter, deferred through Phases 1–3 as "not a prerequisite", **becomes one here.**

## 3. Design

- [ ] **Hook placement.** Append exactly one guarded line to `/etc/init.d/rcS.local`, preserving all
      stock content, that sources `/etc/init.d/rcS.harmony` when both it and the marker
      `/etc/ha-harmony-enable` exist. All our logic lives in `rcS.harmony`; `rcS.local` gets one line.
      Rejected: replacing `rcS.local` (loses Bluetooth and the memory policy); editing `rcS`
      (`DEMO_MODE` must be set before `rcS:84`, and the fewer stock files we touch the better);
      appending to the end of `rcS` (runs after `hal` has already claimed `/dev/i2s`).
- [x] **The hook must be *sourced*, and that dictates its shape.** `DEMO_MODE` has to be set in the
      shell that runs `rcS`, so a child process cannot do it. Consequences, all now enforced by tests:
      `exit` is forbidden (it would terminate `rcS` — no dropbear, no `hal`, no luaworks); a syntax
      error aborts `rcS` the same way; and every variable is `hah_`-prefixed and unset at the end,
      because they leak into `rcS`'s shell.
- [x] **~~Start dropbear first~~ — not needed, and better not to.** The stronger guarantee is that the
      synchronous path **does nothing but read and write small files**: no network, no device, no exec
      of our daemon, no waiting. Then `rcS` provably reaches line 47 and stock starts dropbear itself.
      Starting a second dropbear would be redundant (two instances already run normally) and would put
      an exec on the one path that must not be able to block. `tests/py/test_boot_hook.py` enforces the
      no-blocking rule directly, so it is a checked property rather than an intention.
- [x] **Order inside the hook:** refuse unless daemon *and* config are present → bump the strike
      counter and apply the guard → parse the config → only then `export DEMO_MODE=1` and launch the
      daemon and its watcher in the background.
- [x] **Refuse to go inert.** If the daemon is missing, not executable, or its config has no token, the
      hook leaves `DEMO_MODE` unset so stock `hal` starts normally — and does *not* spend a strike. A
      half-installed hub must be a stock hub, never a dead one.
- [x] **Parse the config, never source it.** `hub.conf` is read with `while IFS='=' read`, honouring
      only `token`, `port`, `hub_id`. Sourcing it would execute arbitrary commands in `rcS`'s shell at
      boot; a test asserts a command planted in the config file does not run.
- [ ] **Self-disable guard** (the `memtester.run` pattern). Keep a counter in `/cache` — not `/data`,
      so a `cache` wipe clears it and it is on a different partition from our binaries. Increment on
      entry; the daemon clears it once it has run healthily for N seconds (propose 120). At
      **3 consecutive unclean boots**, remove `/etc/ha-harmony-enable`, log loudly, and let stock boot.
      Gate: provable by test, not by hope — see §4.
- [ ] **Supervision.** Stock has no respawn supervisor, so decide explicitly: a backgrounded shell
      respawn loop with backoff, or single-shot (daemon dies → hub is inert until next boot).
      Recommend **single-shot for the first landing** — a respawn loop interacts badly with the boot
      counter and can mask a crash-loop as "working". Add supervision only once the guard is trusted.
- [ ] **`luaworks`/`luadraws`.** Gated only on the binary being executable, so `DEMO_MODE=1` does not
      stop them, and they will run with no `hal` to talk to. Investigate what they do in that state
      (`[RO]`, they are already running today) before deciding whether to leave them, and prefer
      *not* renaming stock binaries — a marker or a wrapper is reversible, a rename is a mutation of
      the ro-layer copy-up.
- [ ] **Watchdog: leave OFF for the first landing, then revisit.** Re-enabling it (removing
      `/etc/nowatchdog`) before the self-disable guard is proven turns a crash-loop into a reboot
      loop. Once the guard is trusted, re-enabling it is desirable: it is what converts an OOM or a
      wedge into a recovery rather than a dead hub. Sequence, not preference.
- [ ] **Disable/uninstall path.** `rm /etc/ha-harmony-enable` → next boot is pure stock. That single
      file is also what the guard removes, so the automatic and manual paths are identical. A factory
      reset wipes everything regardless, since all of it is in the rw layer.

## 4. Tasks and gates

- [x] **T1 `[RO]` — rehearse the whole hook on the host. DONE.** `tools/boot/rcS.harmony` plus
      `tests/py/test_boot_hook.py` (16 tests): the guard state machine (clean boot, strikes 1 and 2,
      third boot hands the boot back to stock and removes the marker), a corrupt counter treated as
      clean, the four incomplete-install cases, and the blast-radius rules (`sh -n` parses, no `exit`,
      no applet the hub lacks, nothing blocking on the synchronous path, config parsed not sourced).
      Runs against a temp tree via `HAH_ROOT`, with one case under a PATH holding only hub applets.
      **Gate met:** 16/16 pass, `shellcheck -s sh` clean.
      **Verified by mutation**, because 16 first-try passes prove nothing on their own: making the
      guard never trip, replacing a refusal with `exit`, and sourcing the config instead of parsing it
      each fail the tests that exist to catch them — the config mutation is caught both statically and
      behaviourally (the planted command really does run).
- [x] **T2 `[WRITE]` — install with the daemon deliberately broken. GATE MET, 2026-11-17.**
      Install the hook pointing at a binary that exits non-zero immediately, then reboot **four
      times**. **This proves the guard on the real device before the guard ever has to matter.**
      **Gate:** SSH survives all four boots; `remote.harmony` healthy on boot four.

      **Correction found while running it:** the plan originally said to use an
      `--allow-missing-daemon` flag. That could not have worked — the hook deliberately leaves stock
      alone, spending no strike, when the daemon is *missing*, so the rehearsal would have booted
      stock three times and proved nothing. The flag has been **removed**. The rehearsal needs a
      daemon that is **present and executable but exits 1**: the real binary was moved aside to
      `hub.real` and a 66-byte stub put in its place.

      Also verified on the device before installing, because the installer's safety check depends on
      it: BusyBox 1.13.4's `sh -n` does **not** execute the file (if it had, `sh -n rcS.local` would
      have run stock's boot script live) and does catch a syntax error (exit 2).

      | boot | SSH | marker | strikes | `hal` | hook log |
      |---|---|---|---|---|---|
      | 1 | up | present | 1 | **suppressed** | `starting (boot strike 1 of 3)`, stub exits 1 |
      | 2 | up | present | 2 | **suppressed** | `starting (boot strike 2 of 3)`, stub exits 1 |
      | 3 | up | **removed by the hook** | cleared | **running** | `DISABLED after 3 unclean boots ... booting stock` |
      | 4 | up | absent | none | running | unchanged — the block did not even run without the marker |

      Two things this shows beyond the gate: `DEMO_MODE=1` from a file sourced at `rcS:43` really
      does suppress `hal` at `rcS:84` — the core mechanism of the whole phase, now proven on hardware
      — and the disabling boot **hands itself back to stock on that same boot** rather than waiting
      for a fourth. HA's `remote.harmony` returned to `off` with all 6 devices and both activities.

      **Cleanup, verified:** real daemon restored (md5 `b540739c`, unchanged), `uninstall --purge`
      run, and `rcS.local` is **byte-identical to stock** (md5 `711cb00b`, matching the install-time
      backup). The hub is exactly as found.

      Caveats, stated so they are not mistaken for coverage: these were **soft reboots** over SSH, not
      cold power cycles — the identical `rcS` path, but not a power-loss test. And the hook log is only
      truncated on the starting path, so a disabling boot appends to the previous boot's lines;
      harmless, and arguably useful history.
      - [x] **Installer written and tested: `tools/install_boot_hook.py`** (`status`/`install`/
            `uninstall`, every write behind `--yes`), 26 tests. **Nothing has been run against the
            device.** Kept separate from `deploy_hub.py` on purpose: that tool has a test asserting it
            never writes `/etc/init.d`, `/etc/inittab`, `/etc/rc*` or any `/dev/mtd*`, and weakening
            that invariant to fit this one job would be a bad trade. This tool is the only one allowed
            in `/etc/init.d`, and is narrow: two files there, one marker, nothing else.
      - [x] **Ordering is the safety property, and it is asserted, not just intended.** The marker is
            created **last** and removed **first**, and the appended block is inert without it — so
            every intermediate state, including every way the tool can fail halfway, is a hub that
            boots pure stock. After editing `rcS.local` it runs **`sh -n` on the device**; if that
            fails it restores the backup and never arms, because a syntax error there aborts `rcS` at
            line 43, before dropbear at line 47. Tests cover: marker last, disarm first, backup before
            edit, abort+restore on a broken edit, refusal when `rcS.local` was already broken,
            idempotent re-install, byte-for-byte restore on uninstall, and a truncated write caught by
            read-back verification.
            Also **mutation-verified**: arming the marker first, skipping the restore, and retargeting
            stock `rcS` each fail the test written to catch them.
- [ ] **T3 `[WRITE]` — install for real.** Marker on, real daemon, `DEMO_MODE=1`, single-shot.
      **Gate:** after a cold power cycle with no host attached: our daemon is listening, `/dev/i2s` is
      ours (no `hal`), an `ir_send` reaches a real device, and the hub survives ≥1 h idle. Record RSS.
- [ ] **T4 `[RO]` — the HA half, which is still unstarted.** None of Phase 3's HA-facing path is
      real yet: our component is **not installed in the user's HA**, no config has ever been pushed to
      the device, and `/data/ha-harmony/cache.json` has never been written. A persistent hub that HA
      cannot configure is not useful, so this may deserve to come *before* T3.
      Note zeroconf will not discover the hub (HA's zeroconf does not listen on the VLAN interface
      facing it) — manual host/port is the intended path.
- [ ] **T5 — runbook.** `docs/runbooks/phase4-persistence.md`, including the UART recovery procedure,
      written **before** T3 rather than after.

## 5. Rollback

| step | rollback |
|---|---|
| hook installed, marker on | `rm /etc/ha-harmony-enable`; next boot is stock |
| hook hangs before dropbear | **UART** — this is the case with no remote recovery |
| 3 unclean boots | automatic: guard removes the marker |
| anything at all | factory reset wipes the whole rw layer, then re-root per the Phase 1 runbook |
| flash damage | **not possible by design** — this plan writes no `mtd` device |

## 6. Open questions

1. **Single-shot or supervised?** Recommend single-shot first (§3). Confirm before T3.
2. **`luaworks`/`luadraws` with no `hal`** — leave running, or suppress? Needs the `[RO]` look first.
3. **Watchdog back on?** Recommend after the guard is proven, not with the first landing.
4. **Does T4 come before T3?** A persistent hub HA cannot configure has little value; installing the
   custom component in HA and restarting it is a separate, user-facing decision.
5. **UART access** — confirm the adapter and the header location before T2, since T2 is the first step
   that can plausibly need it.

## 7. Probe results and failure modes (2026-11-17, host-side and read-only)

**T1 probes** (hub stock stack running, nothing written):
- (a) `rcS:40-90` still matches section 1 (rcS.local sourced at 42-43, dropbear 47, hal at 84).
- (b) `/mnt/data/etc/init.d/rcS.local` is not visible from the unioned root (`/mnt/data` listing fails);
  the merged `/etc/init.d/rcS.local` md5 is `711cb00baa096d829b8cfc74400c00d6`, as expected.
- (c) `/data/ha-harmony`: `hub` 846,328 B, `remote_sim` 578,680, `rf_probe` 354,632, `cache.json` 35,018,
  `state.json` 68, `backup/` (empty). `/cache`: only `ota-update.log` (438 B). mtd map unchanged (data and
  cache 5 MB each). No `df` on the hub, so free space is unknown without a staged BusyBox.
- (f) present: `/bin/sleep`, `/bin/kill`, `/usr/bin/killall`, `/bin/pidof`; absent: `df`.
- (e) see `docs/runbooks/phase4-persistence.md`: Pair/Reset button probably handled by `hal` (lost while
  armed, not proven); `luaworks` retries its TCP link to `hal`; no `luadraws` on this hub.
- Static MIPS BusyBox with `df`: built earlier in the external clone, at
  `/home/user/projects/upstream-ref/harmony-ng/build/tmp/busybox-1.36.1-mips/busybox`
  (also `payload/bin/busybox`).

**Failure modes**

| failure | result |
|---|---|
| marker removed | next boot pure stock |
| daemon missing/not executable, conf missing, no token, bad `rf`/`rf_slot`/`stop_lua` value | stock, no strike |
| daemon exits non-zero (bind or radio open failure) | strike stands; third boot disables the hook |
| daemon wedged but alive | counts as healthy; no watchdog (left off), recover by disarm |
| hook syntax error | rcS aborts before dropbear: UART only (guarded by `sh -n` at install and tests) |
| lua stop (`killall`) fails | logged, daemon still starts; `luaworks` retries `hal` forever |
| cold pull mid-boot before 120 s | strike stands (counter in /cache), clears or trips on later boots |
| factory reset | wipes the rw layer; stock |

**Hardware gate (new):**
- G1: three reboots: SSH up, no `hal`, daemon holds `/dev/rfspi`, HA reconnects, Elite key gives IR plus an HA
  event, counter cleared at 120 s.
- G2: forced failure with an exit-1 stub: stock on boot 3, then restore the real binary.
- G3: cold power cycle, including a pull about 10 s into boot.
- G4: `disarm`: pure stock; then re-arm.
- G5: 1 h idle armed: RSS, fds and MemFree flat.
- Then T7: remote hook via `deploy_remote.py install-hook`, with its own 3-reboot, DISABLE and joint
  power-cycle gate.
