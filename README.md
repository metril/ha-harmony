<p align="center">
  <img src="custom_components/ha_harmony/brand/logo.png" alt="Harmony" width="373">
</p>

<h1 align="center">Harmony</h1>

Custom firmware for the Logitech Harmony Hub and Elite remote, a Home Assistant integration that
talks to the hub directly over your LAN, and a **Harmony Editor** add-on to design the setup. The
goal is a Harmony setup that keeps working after Logitech's cloud is gone, with Home Assistant as the
source of truth: devices and IR codes, activities, Home Assistant actions on keys and touch pages, and
remote layouts, all designed in the editor and run by the hub.

> **Status: experimental.** The hub firmware has sent real IR that a real TV obeyed, and the
> integration drives it end to end, but it is not yet a drop-in replacement for stock. Read
> [What works](#what-works) before rooting anything.

## Pieces

| | |
|---|---|
| `hub/` | The hub daemon (C, static, soft-float MIPS for the AR9330). Drives IR through the stock `/dev/i2s` driver, speaks a small WebSocket protocol to Home Assistant. |
| `custom_components/ha_harmony/` | The Home Assistant integration: config flow, activities, per-command buttons, events for remote key presses, Home Assistant actions, remote pages. |
| `addon/`, `harmony_addon/` | The Harmony Editor add-on: visual editor, IR database, config store. |
| `remote/` | Firmware for the Harmony Elite remote (RF link to the hub, touch pages; page format v2). |
| `tools/` | Backup, stock-config extraction and conversion, deployment, and the boot hook. |
| `docs/` | Design, hardware notes (how IR is actually generated on this hub), plans and runbooks. |

## What works

- **IR out of the hub**, generated from your own stock codes with the stock carrier and timings.
  Verified on hardware against a Samsung TV.
- **Home Assistant integration**: activities as a `select`, one `button` per device command,
  `remote.send_command`, key presses as `event` entities, and a hub-side activity engine that
  changes only what needs changing (switching between two activities that both use the TV changes
  its input, it does not power-cycle it).
- **Conversion from stock**: `tools/stock_to_ha_config.py` turns the IR database, device power and
  input definitions, and activities off a rooted hub into an integration config.

## What does not work yet

- **Bluetooth and IP devices.** The stock hub drives some devices over Bluetooth (for example an
  NVIDIA Shield) or the network (Roku). This firmware does neither. Those devices are reported by
  the converter, and are best handled by their own Home Assistant integrations, triggered from an
  automation on the activity select.
- **Boot persistence** is in progress. Today the daemon is started by hand and a reboot returns
  the hub to stock.
- **The Elite remote** firmware is under hardware bring-up: expect rough edges. Hub and remote firmware must be updated together.
- **Zeroconf discovery** only works when Home Assistant listens on the same subnet as the hub;
  manual host/port entry always works. A hub already set up is never moved to a new address by an announcement
  alone: you confirm *Hub moved to ...?* first.

## Installing the integration

1. HACS → ⋮ → *Custom repositories* → add this repository as an **Integration**.
2. Download *Harmony* and restart Home Assistant.
3. *Settings → Devices & services → Add integration*, then enter the hub's host, port (default
   `8099`) and the token the daemon was started with.
4. Install the **Harmony Editor** add-on (see [Editor add-on](#editor-add-on)); confirm its discovery
   card so the hub is linked, then apply a config there. The integration pushes it to the hub only when it
   changes. (Upgrading from < 0.3.0: the YAML file is no longer read; import it in the editor.)

The integration does nothing useful against a stock hub: it needs this firmware's daemon running.

## Editor add-on

The **Harmony Editor** is a Home Assistant add-on with the visual editor (remote canvas, palette, pages),
the IR database and the config store; it feeds the integration, which stays the hub's runtime client.

- Install: add this repository's URL in *Settings -> Add-ons -> Add-on store -> Repositories*, install
  *Harmony Editor*, start it and open it from the sidebar. It needs the integration >= 0.3.0 and Supervisor
  2026.10.3 or newer.
- **The add-on image is published by the release workflow (one `vX.Y.Z` tag).** If a release failed before
  that step, the store cannot install the add-on until the workflow is re-run.
- Read next: [`addon/DOCS.md`](addon/DOCS.md) (install, link, backups, troubleshooting),
  [`docs/ha-config.md`](docs/ha-config.md) (the config format) and
  [`docs/addon-api.md`](docs/addon-api.md) (the add-on's HTTP API).

## The hub side

Running the firmware needs a **rooted** hub. Rooting uses the third-party
[harmony-ng](https://github.com/iranl/harmony-ng) tooling; nothing from it is part of this
firmware. Take a full backup first: `tools/device_backup.py` images every flash partition and
verifies each one. The runbooks in [`docs/runbooks/`](docs/runbooks/) describe each step,
including how to get back to stock.

This can leave a hub in a state that needs a serial (UART) console or a factory reset to recover.
Don't do it to a hub you can't afford to lose.

## Development

```sh
uv run pytest -n auto        # Python suite, includes HA integration tests against the hub simulator
cmake -S . -B build-host -DHP_BUILD_TESTS=ON && cmake --build build-host && ctest --test-dir build-host
tools/fetch_toolchains.sh    # pinned cross toolchains for the hub (soft-float MIPS) and remote
```

CI also runs the C suite big-endian, soft-float, under QEMU, because the hub's CPU has no FPU and
host tests cannot catch that class of bug.
