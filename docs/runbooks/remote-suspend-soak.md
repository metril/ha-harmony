# Remote suspend and battery guard: bench gate

Suspend to RAM is opt-in. Start the app with `HARMONY_SUSPEND=battery` (suspends only when the gauge's `powersource` is exactly
`battery`) or `HARMONY_SUSPEND=always` (bench only: also on USB or the cradle). Unset or anything else: never suspends.
The app never writes any CPU-frequency node (enforced by `tests/py/test_remote_no_cpufreq.py`).

Behaviour: screen idle 10 s dims, 30 s switches the backlight off; 10 s later (`RA_SUSPEND_AFTER_MS`) the panel is blanked
(`FB_BLANK_POWERDOWN`); the MSP430 timed wake is armed (`3600` written to `1-0034/wakeup`, as upstream does) and `mem` is
written to `/sys/power/state`. If the alarm cannot be armed the suspend is not attempted (a remote asleep with no alarm would
never run the battery guard) and it backs off like a refusal. A refused suspend backs off 60 s. Wake sources: the hard
buttons, the motion sensor (LIS3DH), and the hourly alarm. Touch does not wake the remote (the kernel masks the Clearpad
during suspend). The resume logs what woke it (`1-0034/wakeup_event`: `key`, `alarm`, `none`...). An hourly alarm wake
polls the battery guard at once, then sleeps again after the 8 s hold. `/dev/rfspi` is deliberately NOT closed or reopened
around the suspend (upstream does not either: a resume keeps it); step 1's 5 s re-link bound is what verifies that on the bench. On resume the panel is unblanked, the screen
goes active (the waking key goes on to the hub), the link announces itself again (remote_info + battery), the battery guard
polls at once, and sleep is held 8 s. The sleep length is the wall-clock delta less the monotonic delta (logged as
`resumed after N ms asleep`).

Run on a bench remote with a serial/log tap, never the only unit. Another agent may own the bench: take it first.

## 1. 20 suspend/resume cycles on USB (go/no-go: all 20, RF re-link < 5 s)
1. On USB power start the app with `HARMONY_SUSPEND=always` and `--rf spi`; start log capture of stderr.
2. Cycle: leave the screen alone 40 s + 10 s (watch for `suspend to RAM`), press a hard key, note the time until the hub shows
   the remote attached again (`remote_info` seen) and a pushed page redraws. Required: < 5 s, key reaches the hub, page intact.
3. 20 times, mixed idle lengths (1 min to 10 min asleep). Log `resumed after ... ms` each time: gap must be plausible.
4. Fail on: any hang, dead keys, blank panel after wake, link not back in 5 s, a `suspend refused` that repeats.
5. USB (`usb0`) drops while suspended: ssh back in after the wake, do not read that as a failure.

## 2. Cradle charge transition
1. `HARMONY_SUSPEND=battery`, on battery: it suspends after the idle time. Place it on the cradle while asleep: it must wake
   (motion or a button; the cradle itself is not a known wake source, so note which one it was), show the page, and the status bar shows charging.
2. On the cradle, idle for 5 min: it must NOT suspend (log has no `suspend to RAM`); screen off, backlight 0.
3. Lift it off the cradle, idle: it suspends within about 30 s (`RA_SUSPEND_RECHECK_MS`) of the unplug plus the idle time.

## 3. Overnight battery drain
1. Full charge, off the cradle, `HARMONY_SUSPEND=battery`, link up. Record soc and time, then leave 8 to 12 h untouched.
2. In the morning wake with a key; compare soc and the logged `total ms asleep`. Report %/hour. Upstream's reference is
   about 1.3 %/hour in deep standby; far above that means something kept the SoC awake.
3. The hub's held pushes made while it slept must arrive after the wake (change a tile or page during the night).

## 4. Forced low-battery guard
1. Make a directory with files `soc` (`2.0`), `powersource` (`battery`), `battcharge` (`-120.00 0x0000`) and start the app with
   `HARMONY_FAKE_BATT_DIR=<that dir>`. Reads of the three battery nodes come from those files; it is off when unset.
2. Within the first poll (60 s, or at the first wake) the log shows `powering off` and `/sbin/poweroff` runs once: the remote
   shuts down cleanly. With `powersource` = `cradle` or `battcharge` > 0 nothing happens.
3. Also check it across a suspend: start `HARMONY_SUSPEND=battery` with healthy files, let it sleep, change `soc` to `2.0`,
   wake it: it must power off at the first poll after the resume.
4. Then remove the env and confirm normal boot.

## Not covered yet
- No touch wake (the kernel masks the touch panel during suspend), by design.
- Whether the LIS3DH wakes a suspended remote is read from the kernel survey, not yet measured on this build: check it in step 1
  (shake a suspended unit) and record it.

## Bench note (2026-11-19)

The wake alarm is fixed at 3600 s: there is no test override, so a suspend cycle on the bench needs a hard-key press (or a 1 h wait). `1-0034/wakeup` (rw, reads 3600) and `wakeup_event` (ro, reads `none`) exist. The gate was not run unattended.
