# Bench gate: custom theme transport

Run on the real hub and remote after flashing both (hub with `theme_push`, remote announcing caps bit 3). Design:
`docs/proto/themes-transport.md`. Use delegated bench operation; nothing here needs a version bump.

Setup: a config with `theme: dusk` (a user theme extending midnight with a visible accent change, some palette and free
colours) and, for the preset pass, `theme:` set to each of default, midnight, daylight, high_contrast in turn. The *Remote 1
page format* sensor should list `theme_custom` and, for the custom pass, show `theme: {sent: true, acked: true}` once done.

## Checklist (tick each; note the time to full pages)

- [ ] **Each preset** (default, midnight, daylight, high_contrast): set, wait for pages; every page uses that preset; no theme_push
      is sent (diagnostics `theme_pushes` unchanged).
- [ ] **One custom theme**: pages arrive in the custom colours, never in a preset's first (watch the first screen after the
      config is applied: "Receiving", then the final look, no flash of the base preset); `acked` turns true.
- [ ] **Reboot the remote** (custom theme active): after boot the remote shows "Receiving", then the custom look again; the
      integration re-sends (theme_pushes +1).
- [ ] **Suspend / resume** the remote (custom theme active): the page and theme are intact after resume without a re-push; no
      visible change; if the integration re-sends, the result is identical (idempotent).
- [ ] **Restart the hub** (`hub` daemon or reboot): the remote keeps its page and theme; nothing is re-sent when the remote reports
      the full set; after the hub is back the look is unchanged.
- [ ] **Config change mid-push**: apply theme A, and while the remote shows "Receiving page k of N" apply theme B (change the
      accent). The remote ends on B only; at no point do pages of A and the theme of B (or the reverse) mix on screen.
- [ ] **Link loss**: walk the remote out of range during a push, return: the set completes without a manual re-push (hub resends
      or the 20 s check re-sends); no page is ever drawn with a missing theme.
- [ ] **Edit the theme only** (same pages): the remote repaints with the new colours.
- [ ] **Older hub firmware** (if available): the integration logs "does not know theme_push" once, pages show the base preset.
- [ ] Visual check on the real panel for each preset and the custom theme: contrast, pressed colour, outline on near-ground
      fills, tile off / stale / unavailable colours, status bar and toast under the custom theme.

Record the result, the hub and remote build ids, and any frame that showed a wrong theme.
