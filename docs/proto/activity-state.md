# Hub-executed activities, device state and key hold

Implemented in `hub/engine/activity.{h,c}` (config format 2 only; format 1 keeps the immediate
`steps`/`off` behaviour). Works with Home Assistant down: the hub tracks every device itself.

## Tracked state
Per device: `power` (on/off) and `input` (a name from `devices[].inputs`, or unknown). A fresh
hub, a missing/corrupt state file, or a new device starts as off / input unknown. Powering a
device off makes its input unknown. Devices without power codes are only tracked.

## Transition (start activity B)
Planned from the tracked state at request time, executed one step per timer (never blocking):

1. power OFF every device that is on and not used by B;
2. power ON every B device that is off (a toggle code is never sent to a device already in the
   wanted state; discrete codes are only sent when the tracked state differs);
3. select inputs (device order) where the tracked input differs or is unknown; an input step waits
   until the device's `settle_ms` after its power-on or last input select has elapsed;
4. B's extra `steps` (wait for every device's settle time).

`Off` powers down everything that is on, then runs the current activity's `off` steps.
Tracked state is updated when each frame goes out. Steps re-check the tracked state when they
run, so a `set_device_state` correction during a transition skips what became redundant.

**No overlapping frames.** The next IR step starts no earlier than the previous frame's duration
(for SIRC: all 3 copies, `2 * period + frame`) plus `HP_ACT_GUARD_MS` (50 ms). This line-busy
time persists across plans.

**Interruption.** A new start/Off while a transition runs cancels its remaining steps. Frames
already sent stay in the tracked state; the new plan is built from that state, and pending
settle times are kept (a device powered on 20 ms ago is not sent an input yet). The activity
whose `off` steps run is the last requested one (the interrupted target counts as current).
While a transition runs, non-activity key presses are dropped and the `ir_send` verb answers
`{ok:false,"transition in progress"}`; HOLD/RELEASE are harmless. A reload (`set_config`)
cancels the plan; the interrupted target is never reported or persisted: the last *completed*
activity stays (re-found by id, none if it no longer exists) and device state stays what was
actually sent. An IR send failure aborts the plan and reverts to the reported activity.

## What is reported
* `state_update {activity, power}` is sent when a transition **completes** (also over RF). While
  one is running, the last completed state is what `state_update` / the post-`hello_ack` state
  shows. An interrupted transition is never reported; its successor is.
  `Off` reports `{activity: 0, power: false}`.
* `device_state {device, power, input?}` (`input` omitted = unknown) goes to all authenticated
  clients whenever a tracked device changes: each executed step, `set_device_state`, and on
  `set_config` for devices that are new or changed. After `hello_ack` a client receives
  `state_update` and then one `device_state` per device.
* `command` device `hub`, `set_device_state`, args `{"device": "<id>", "power"?: bool,
  "input"?: "<name>" | null}` sets the tracked state **without sending IR** and pushes
  `device_state`. `input: null` (or `""`) = unknown; `power: false` also makes the input unknown.
  Unknown device/input, or none of power/input: `command_result {ok:false, message}`.
* `start_activity`/`power_off` reply `command_result {ok:true}` when the request is accepted.

## State file (`hub --state PATH`, separate from `--cache`)
`"HPST"` | version u8 (1) | config hash u32 BE (same CRC-32 as `hello_ack.config_hash`) |
activity id (u8 length + bytes, length 0 = none) | device count u8 | per device: id (u8 len +
bytes), power u8, input (u8 len + bytes, length 0 = unknown) | CRC-32 (IEEE) u32 BE over all
preceding bytes. Written with `hp_atomic_write`.

Debounced: the first change after >= 2 s of quiet is written immediately; further changes in
that window coalesce into one trailing write at its end (at most one write per
`HP_ACT_PERSIST_MS` = 2 s); a pending change is flushed on SIGTERM/SIGINT. The persisted activity
is the last *completed* one. At startup (after `--cache` is loaded) a missing, corrupt,
other-version or other-config-hash file is ignored with a log line and the hub starts all
off/unknown; it never refuses to start. After a restart `hello_ack`'s state and `device_state`s
show the restored state.

## Key hold
The remote sends `key_event` PRESS, then a HOLD **every 400 ms** (`HP_IROUT_HOLD_INTERVAL_MS`)
while the key stays down, then RELEASE. The hub repeats the key's IR frame from the first HOLD
until RELEASE. The 2 s safety cap (`HP_IROUT_HOLD_CAP_MS`) is measured from the **most recent**
HOLD (each HOLD restarts it): a real long hold keeps repeating, a lost RELEASE (or a dead
remote) stops the repeats within 2 s of the last HOLD, and later stray HOLDs do not resume them
(a new PRESS is needed). `remote_sim --hold KEY,MS [--hold-interval-ms N]` emits exactly that
sequence.
