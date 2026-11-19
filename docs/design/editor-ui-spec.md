# Harmony Editor: web UI spec (visual-first)

The UX and visual spec for the add-on's editor (`harmony_addon`), served in the Home Assistant ingress
iframe. Mockups: [editor-ui-mockups.html](editor-ui-mockups.html). The API is `harmony_addon/app.py`,
plus the additions in section 7.

Items marked **[decided 2026-11-18, revisit on request]** were decided while the user was away.

**Principle: the remote is the canvas.**
- The user works on a drawing of the Harmony Elite. They select keys and on-screen widgets on it and drop
  commands, activities and Home Assistant entities onto them.
- Forms appear only in popovers and in the inspector. Tables survive only as a secondary "List view".

**What carries over unchanged from the previous spec:** app shell, router, draft store, draft bar, apply
dialog, api client, tokens and constraints.

## 1. Navigation and layout
- **Header (48 px):** title, hub picker, Import YAML, Export YAML.
  - The hub picker is shown only when `GET api/v1/hubs` returns more than one hub. **[decided 2026-11-18, revisit on request]**
  - The hub is a `?hub=` value that is part of the URL.
- **Rail (76 px, icon above label):** Overview · Remote · Activities · Devices · History.
  - A red dot marks a section with errors.
  - The IR library is reached from Devices (Add device) and from the palette search.
- **Draft bar:** unchanged.
  - State dot + "Draft · N changes not applied" + "saved …", or "No changes since rev N".
  - "Valid" or "N errors · Show" chip.
  - Meters (amber at 80%, red at 100%): on a current hub one "Config size X of Y KiB on this hub" (its memory budget; no command/IR-pool counts); on old firmware config KiB, IR pool, commands /256. Pages /32 always.
  - Discard draft; Review & apply (disabled, with a reason, while there are errors).
- **Phone:** the rail becomes a menu button; the draft bar becomes a top-bar chip plus Apply; palettes and inspectors become bottom sheets.
- **First run** (no draft and nothing applied): Overview shows the bare remote with "Import YAML file" and "Add your first device".
- **Autosave:**
  - Edits PUT the whole draft about 600 ms after the last change, with `If-Match: <rev>`.
  - A 409 conflict shows "Changed in another tab: Reload / Overwrite".
  - Undo/redo (Ctrl+Z / Ctrl+Shift+Z) works on the client's draft history.

## 2. Interaction model (applies everywhere)
- **Sources:** anything in a palette — device command chips, input chips, activities, Home Assistant entities and services, widget types, page templates.
- **Targets:**
  - hard keys on the remote;
  - screen cells and widgets;
  - the "When tapped" slot in the inspector;
  - activity chain slots and timeline gaps;
  - "who handles it" zones.
- **Every assignment can be made three ways:**
  1. Drag the source onto the target.
     - The target pulses: a dashed teal glow.
     - A label previews the result: "Volume + ← AV receiver · vol_up, overrides Default".
     - Esc cancels the drag.
  2. Click (or tap) the source, then click the target.
     - All valid targets are outlined.
     - A banner reads "Tap a key for vol_up · Cancel".
  3. Select the target, then pick in a popover anchored to it.
     - The popover has: Override / Default / Nothing, the current action with Send IR, a search box, and ranked suggestions (the same device's commands, other layers' choices, activities, entities).
- **Keyboard:**
  - Tab moves through keys and widgets in reading order; Enter opens the popover.
  - In the palette, Space picks a chip up; arrows move between targets; Enter drops; Esc cancels.
  - Each step is announced through a live region.
- **Undo:** every drop is one undo step, and a toast says "Assigned … · Undo".
- **Drag implementation:** pointer events, not HTML5 drag and drop, which does not work on touch and is unreliable inside iframes.
  - Use `setPointerCapture` and `touch-action:none` on sources and the canvas.
  - On touch, drag starts on a long press (350 ms); a plain tap means click-to-assign.
  - The drag shows a ghost chip, and the canvas auto-scrolls near its edges.

## 3. Screens
### 3.1 Remote studio (main screen)
**Layout:** palette (292 px) | canvas | inspector (300 px).

**Canvas toolbar**
- **Layer switcher (pill bar):** Default · each activity · …
  - Switching layers re-colours the keys and swaps the screen to that activity's page (the page whose `activity` is set to it, otherwise the Home page).
- Whole remote | Screen (the focus mode).
- X-ray toggle.
- List view.
- Zoom: Fit (default), 1×, 2×.

**The remote drawing**
- One SVG component, `elite-remote`, drawn from a data file `elite-layout.ts`. For each of the 35 keys it holds: key name, logical code, group, shape, path or rect, and label.
  - Fixed parts: the body, the glass with the 240×320 screen at (30,56) in a 300×900 viewBox, and the Home/Favorites strip.
  - Key positions follow `docs/hardware/remote-elite.md` and must be checked against the real device (see section 8).
- **Key states in the current layer:**
  - overridden: teal fill and outline;
  - from Default: a small grey dot;
  - does nothing: dimmed;
  - selected: white ring with teal glow;
  - drop target: pulsing dashed teal;
  - error: red outline.
- Hovering or focusing a key shows a tooltip, "Volume + · AV receiver · vol_up".
- **Screen:**
  - Shows the live compiled page from `POST config/preview` `pages[]`, drawn by the shared `remote-page` renderer in the direction-B palette.
  - Clicking a widget selects it.
  - In Whole-remote mode, double-click (or Enter) switches to Screen mode.

**Palette tabs**
- **Devices:** collapsible device cards (illustration, name, model). Command chips are grouped as Power · Volume · Navigation · Transport · Numbers · Inputs · Other.
  - Groups are guessed from command names and can be overridden per command; the override is stored in `editor:` metadata.
  - Chips already used in this layer have a teal outline.
- **Activities:** activity chips plus "All off".
- **Home:** live Home Assistant entities grouped by area, each with an icon and its current state (`ha/entities`); services are reached through an entity's ⋯ menu.
- **Widgets:** Button, Tile, Label, List, plus the page templates.
- Search filters every tab at once.

**Inspector**
- Nothing selected: a layer summary with counts per state, "Who handles it" (read-only, linking to Activities), and the free keys as chips (click one to select that key).
- Key selected: the same content as its popover, in full.
- Widget selected: see 3.3.

**Key semantics**
- Default layer = top-level `keys`. Activity layer = `activities[].keys`, which overrides Default.
- "Default" in an activity layer removes the override; "Nothing" writes `{none: true}`.
- Holding Alt while dropping applies to every activity layer (after a confirm toast with Undo).

### 3.2 X-ray
- The X-ray toggle labels every key at once, with leader lines to the left and right, in the layer's colours.
- The same rendering, scaled down, is reused in the Overview "changes" card and the apply dialog, with changed keys outlined amber.

### 3.3 Screen editing (WYSIWYG, same canvas)
**Screen mode**
- Zooms the canvas to the display at 2× (3× optional) with a faint cell grid, still framed by the remote's top.

**Widgets on the canvas**
- Drag to move; corner handles resize; both snap to cells, and overlaps are refused live (shown as a red ghost).
- A floating mini toolbar above the selection: Duplicate · Icon · Send the action · Delete.

**Drops onto the screen**
- A command dropped on an empty cell → a button with that action.
- A light, switch, fan or cover → a tile bound to the entity that toggles it on tap (`light.toggle`, …).
- A sensor → a read-only tile.
- A scene or script → a button.
- An activity → a start button, with a `bind` so it shows teal while the activity is running.
- A widget type → an empty widget whose popover opens straight away.
- Anything dropped on an existing widget replaces its "When tapped" action.

**Keyboard on the canvas**
- Tab between widgets; arrows move; Shift+arrows resize; Del removes; N adds a widget at the first free cell (shown as a ghost cell).
- The canvas is `role=application` with a live region.

**Page strip (under the canvas)**
- Real thumbnails rendered by the same `remote-page` renderer at 84 px, in order.
- Each thumbnail shows the page name, a byte bar (amber at 85% or more, red when over), and an error dot.
- Drag, or Alt+←/→, to reorder.
- "+" opens the template picker: Activities home · Activity control · Number pad · Lights · Device list · Blank.
  - Templates are pre-filled from the config: one row per activity; the activity's devices' commands; the Home Assistant lights in the selected area.

**Inspector (widget)**
- Label with a live byte counter (n/24 B).
- "When tapped" slot: shows the current action with Send IR; accepts drops; click it to open the action picker.
- Role swatches drawn in the remote palette: default / primary / secondary / danger.
- Shape swatches: card / row / pill / monogram.
- Icon grid: the 44 stock icons from `icons.json`, with search and "None".
- Icon only toggle.
- Tap goes to page: No / Next / Previous / Home / a page. This replaces the tap action.
- Id and position are under "More".

**Inspector (page)**
- Byte meter by the v2 rule: header 11 + title bytes; each widget 5 + its non-default optional bytes + label bytes; at most 251. When over, it shows the compiler's own message.
- Kind: Normal / Home / Favorites.
- Title.
- Show when <activity> starts.
- Back chevron (Exit goes Home).
- List mode, split into pages: the strip shows the paged group as "1/3 2/3 3/3", and the screen bar shows "idx/cnt".

### 3.4 Activities: signal chain
**Pill bar:** one entry per activity, plus New.

**Chain canvas**
- Device cards (illustration, name, power mode, settle) laid out left to right as source → switch → display.
- SVG wires between the cards, with an input pill on each wire: the input the downstream device selects.
  - Click a pill to open an input picker; dropping an input chip on a wire sets it.
- A device is added by dropping it on the "Drop a device" slot (max 8) and removed with its card's ×.
- The order is inferred from the inputs and can be dragged; it is only visual and is stored in `editor:` metadata.

**Sequence timeline**
- A When it starts / When it stops switch.
- Steps are a horizontal row of chips:
  - auto steps (power on, set inputs) are dashed and locked;
  - your own steps are solid and draggable, and accept dropped commands;
  - settle delays are drawn as dotted gaps whose length is proportional to the time, labelled in seconds.
- Shows the total time and "N / 16 own steps".
- Keyboard: Alt+←/→ moves a step.

**Who handles it**
- A mini remote with four zones around it: Volume · Channels · Navigation · Transport.
- Drop a device chip on a zone, or click the zone and pick. The zone's keys light up on the mini remote and are filled by name match; keys with no match are listed ("no stop command, Stop stays free").
- Stored in the top-level `editor:` metadata, which the compiler ignores, so it survives reload and YAML export. **[decided 2026-11-18, revisit on request]**
- A key overridden on the remote wins.

**Open on remote:** jumps to the studio with this activity's layer selected.

### 3.5 Devices
**Gallery**
- Cards show the device-type illustration (inline SVG), name, brand/model or source, and status chips:
  - Power on + off (green), Power toggle only (amber, "may get out of step"), No power control;
  - commands and inputs counts;
  - errors.
- An "Add device" card.

**Device page (keypad)**
- Header: illustration, name, model, id, settle.
- Commands appear as large keys (48 px) grouped by function: Power, Volume (+ − Mute), Inputs, Navigation (a d-pad layout of ▲◀OK▶▼), Transport, Numbers (3×4), Other.
- Each key has a separate round amber Send button at its right edge.
  - Clicking the key itself opens the code-editor popover: name (31-byte counter), protocol, pronto, raw, file, port toggles, Move to group.
  - Error keys are red with their message under the label.
  - "+ input" and "+ command" placeholders sit in each group.
- **List view toggle:** the previous spec's command table, which keeps bulk select, Set IR ports, Rename and Delete.

**Send IR safety (everywhere)**
- Amber is used for Send IR only, and the control is always labelled or has an `aria-label`.
- It is a separate target and never fires on hover, row or key click, or Enter in a field.
- Cooldown is shown as a check plus a countdown; `ir/test` is rate-limited server-side.
- Works on draft codes. **[decided 2026-11-18, revisit on request]**
- Disabled while the code is invalid.

### 3.6 Add-device wizard (full page, progress dots)
1. **What are you adding?**
   - Eight tiles with inline illustrations: TV, AV receiver, Soundbar, Streaming box, Game console, Projector, Blu-ray/DVD, Something else.
2. **Brand:**
   - A large search box (`ir/brands?q=`) above a grid of type tiles, each with the brand name in big type and its model count. No logos.
   - Virtualised; recents first.
   - Also offered: Paste a Pronto code, Upload a Flipper/SmartIR file (`ir/parse`).
3. **Power test** (`ir/candidates`):
   - Headline "Point the hub at your <brand> <type>", with "Code n of N · used by N models".
   - One 180 px amber Send button.
   - The question "Did the TV turn on or off?" with Yes / No, next code.
   - Shortcuts: Space / Y / N. Cooldown text. "I know my model" jumps to model search.
4. **Model** (`ir/models`), filtered by the matched codeset.
5. **Review:** the keypad view of the commands it found, with Send on each, a name, and Add device (draft only).

**Offline:** a red banner ("The IR archive can't be reached. Cached brands still work. Retry"). Per-item errors are not fatal.

### 3.7 Overview
- **Your remote:** the remote, showing the Home page as it will look (Open in studio).
- **Activities:** a card per activity with its mini device chain and input pills (Changed chip; click to open).
- **Since rev N:**
  - Before/after thumbnails for each changed page, with changed widgets outlined amber.
  - A mini x-ray of changed keys per layer, with a text list.
  - Device and command changes as text.
- **Home Assistant in this config:**
  - The entities and services the draft uses, with live state; for information only. **[amendment 9]**
- First run: see section 1.

### 3.8 Apply (dialog; concept unchanged from the shell build)
- **Content:**
  - Before/after page thumbnails, plus a mini x-ray of the keys.
  - Diff list: Added / Changed / Removed chips.
  - Meters and the config hash.
  - Cancel / Apply to hub. Focus is trapped; Esc cancels.
- **Progress row** (`aria-live`): Validated → Saved as rev N → Waiting for Home Assistant → On the hub → Remote pages updated.
  - The UI polls `GET api/v1/config/status?hub=`, which the integration's `POST api/v1/bundle/ack {hub, rev, result, detail}` feeds. **[decided 2026-11-18, revisit on request]**
- **No acknowledgement within 30 s:**
  - The dialog says: "Saved as rev N. Home Assistant hasn't picked it up yet. Is the Harmony integration set up and linked to this add-on?"
  - It links to setup help and offers "Keep waiting in background".
- **Ack with `error`:** a red banner with the detail, "The hub keeps rev N−1", and "Revert to rev N−1".
- **422 response:** the dialog closes and jumps to the error targets.

### 3.9 History, Import/Export
- **History:** the last 5 revs as cards, each with a remote thumbnail and a change summary.
  - Actions: View diff (the same visual diff as Apply), Restore as draft, Revert to previous (needs a confirm).
- **Import:** YAML upload; asks before replacing an existing draft.
  - A 422 `ir_files_unresolvable` lists the files, each with "Upload this file" (`ir/parse`).
- **Export:** downloads the draft or applied config with `?source=`.

### 3.10 Validation errors
- All errors come back as a list. Each `{path, msg}` is resolved to a visual target:
  - a key (red outline in the right layer);
  - a widget or page (red outline, a red dot on the thumbnail);
  - a command key (red key);
  - a chain card or step (red).
- Errors also count on the rail dot and the draft-bar chip.
- "Show" walks through the errors: it switches layer or page and selects the target.
- Errors with no path go in a banner.
- Messages are rendered as text only.

### 3.11 List views (secondary)
- **Devices:** the command table from the previous spec (bulk edit).
- **Remote:** a key table with all layers as columns: key | Default | Watch TV | Stream …; cells use the same popover picker.
- These are full alternatives for keyboard and screen-reader users and are kept in step with the visual views.

### 3.12 Home Assistant action (in every action picker) [amendments 6 + 9]
- **Service:** `domain.service` from `ha/services`, limited to the allow-list: light, switch, scene, script, media_player, remote, fan, cover, climate, input_boolean, input_button, button, number, select.
- **Targets:** an entity picker with search, area and live state, from `ha/entities`. Free-text entity ids (checked against `^[a-z_]+\.[a-z0-9_]+$`) are the fallback when the route answers 503.
- **Data fields:** rendered from the service's field list.
- **Keys only:** "Repeat while held".
- Tiles get the same "When tapped" action.

## 4. Design system (tokens unchanged; additions marked +)
**Tokens (light / dark)**

| Token | Light | Dark |
|---|---|---|
| bg | #F3F5F6 | #0E151C |
| sf, sf2, sf3 | #FFF, #EEF1F3, #E2E7EB | #162029, #1D2934, #273541 |
| line | #D3DAE0 | #2E3D4B |
| tx, tx2, tx3 | #14202A, #4A5866, #5F6C78 | #F2F6F8, #AEBAC5, #8B99A6 |
| acc (on-acc) | #0B7A6C (#FFF) | #20C8B0 (#06201C) |
| ir (on irs) | #8A5300 on #FFF0D2 | #F8C040 on #3A2E12 |
| bad | #B3261E | #FF7A6B |
| ok | #1E7B3A | #5BD58A |
| focus | = acc | = acc |
| + cv (canvas, with an 18 px dot grid in `dot`) | #E6EBEE | #0A1016 |
| + dot | #CBD3D9 | #1C2832 |
| + lift (drag shadow) | 0 10px 28px rgba(16,24,32,.28) | 0 12px 30px rgba(0,0,0,.6) |

- Dark tokens apply under `@media (prefers-color-scheme: dark)` with `:root:not([data-theme="light"])`, and under `[data-theme="dark"]`.
- **+ Remote body palette** (a physical object, the same in both themes): body gradient #15191D → #2B3137; keys #3C444B → #252B30; glass #05080B; key labels #9AA6B1; colour keys #B8423A, #3E8E52, #BC9727, #3B69B8.
- **Remote screen palette** (direction B): #101820 / 182430 / 202C38 / 304050, text #F8FCF8 / A8B4C0, accent #20C8B0, warm #F8C040, bad #F85848.
- **Overlay states on the remote:**
  - override: fill rgba(32,200,176,.28) with a #20C8B0 outline;
  - drop target: the same, dashed, with a glow;
  - selected: a #FFF ring;
  - changed: #F8C040;
  - nothing: rgba(5,8,11,.55).

**Type**
- Font: system-ui; codes in ui-monospace.
- Sizes 12 / 13 / 14 (base, 1.45 line height) / 16 / 20 / 22 / 24. Weights 400 / 500 / 600 / 700.
- Inside the remote screen, sizes use `cqw` units, so one renderer serves 84 px thumbnails, the 1× studio and the 2× editor.

**Spacing and shape**
- 4 px base: 4, 8, 12, 16, 20, 24, 32.
- Radii 6 / 10 / 16; chips 13.
- Controls 34 px tall (28 small); keypad keys 48 px; 44 px touch targets.

**Components**
- **Unchanged:** button, Send IR, input, segmented control, chip, meter, dialog, banner, toast.
- **+ New:** `elite-remote`, `remote-page` (renderer), palette, command chip (idle / used / picked / dragging), entity row (with live state), popover picker, layer pill bar, page strip and thumbnail, template picker, swatch grid, icon grid, chain card, wire with input pill, timeline step and gap, handler zone, keypad key with Send.

**Icons**
- One inline SVG sprite: 24 px grid, 1.8 stroke, `currentColor`.
- Page-widget icons come from `icons.json`.
- Device-type illustrations: 64×48, 2 stroke, inline SVG.

**Motion**
- 120–180 ms ease-out on selection, drop and layer switch: keys cross-fade between layer colours.
- The drop-target pulse is 1.2 s and turned off under reduced motion.
- No decorative animation.

**Accessibility**
- Focus ring: 2 px accent with a 2 px offset.
- Every key is `role=button` with a full `aria-label` ("Select, Stream layer: Streaming box · ok, override").
- The layer bar is a `tablist`.
- The palette is a `listbox` per group; a drag announces both its source and its target.
- Contrast at least 4.5:1.

**Copy**
- Plain verbs: "Apply to hub", "Send IR", "Use Default", "Drop a device".
- A drop preview always says the key, the action and the effect.

## 5. Technical constraints (unchanged, plus drag)
**Build and CSP**
- Lit 3 + TypeScript + Vite, built to a single JS file and a single CSS file. The current CSP (`style-src 'self'; script-src 'self'`) is enough.
- Component styles use Lit `static styles` (adopted stylesheets); global tokens go in `app.css`.
- Positions are set only through `styleMap`; never bind a `style` attribute, `<style>` tags, `innerHTML` or `unsafeHTML`.

**`api.ts`**
- URLs relative to `document.baseURI`.
- JSON content type and `X-HH-CSRF: 1` on every mutating request; YAML content type on import.
- Draft PUTs are queued, each with `If-Match`; the UI updates immediately and rolls back with a toast on failure.

**Lists:** `@lit-labs/virtualizer` (bundled) for brands, models and entities. No external assets of any kind.

**Preview pipeline**
- After each draft change (debounced 300 ms), call `POST config/preview` with the draft in the body.
- Cache the result by draft rev; thumbnails and the screen render from its decoded `pages[]`.
- Key layers are resolved on the client from the config: `keys` merged with `activities[].keys`, plus "who handles it".

**Drag**
- Pointer-event based (see section 2).
- One `drag-controller` for the whole app, with `dragstart` / `over` / `drop` events and a registry of targets that accept by kind.
- The click-to-assign and keyboard paths call the same `assign(source, target)` function.

## 6. Build order
1. **Shell (already in progress), plus the remote studio for hard keys:**
   - `elite-remote` + `elite-layout.ts`, and the live screen preview through `remote-page` from `config/preview`.
   - Palette: Devices and Activities tabs.
   - Layer pill bar.
   - Drag, click and popover assignment of hard keys for the Default layer and activity layers, with key states, tooltips and undo.
   - **This is the first shippable step.**
2. **X-ray mode** and the key list view (the accessible alternative).
3. **Screen WYSIWYG:**
   - Screen mode, move/resize by pointer and keyboard, drops onto the screen, page strip with thumbnails and reorder, template picker.
   - Inspector: label, "When tapped", page settings.
4. **Page format v2 visuals:** role and shape swatches, icon grid, icon only, nav target, list mode and paged groups, v2 byte rule. Needs page format v2, which is being built now.
5. **Home Assistant palette tab and Home Assistant action** (`ha/entities`, `ha/services`); entity tiles. Needs amendment 9.
6. **Activities:** signal chain, input pills, add/remove devices.
7. **Activities:** sequence timeline, and "who handles it" zones with the mini remote (`editor:` metadata).
8. **Devices gallery and keypad page;** code-editor popover; Send IR (`ir/test`).
9. **Add-device wizard:** type tiles, brand and model search (`ir/brands`, `ir/models`, `ir/device`).
10. **Power-test loop** (`ir/candidates`), and Pronto/Flipper/SmartIR import (`ir/parse`).
11. **Overview:** visual summary, before/after diff, Home Assistant usage list. Reuse the diff in the Apply dialog and in History.
12. **History cards,** and import/export polish.
13. **List views:** the devices command table with bulk edit, and the complete key table.
14. Learning from the original remote (needs a learn endpoint).

## 7. API
**Existing**
- `GET config`, `PUT config/draft`, `POST config/{validate,preview,apply,revert,import_yaml}`, `GET config/export_yaml`, `GET health`.

**Being built** **[decided 2026-11-18, revisit on request]**
- `If-Match` on draft PUT, with 409 on conflict.
- Validation returns all errors.
- `GET hubs`.
- `GET ir/brands?q=`, `ir/models?brand=&q=`, `ir/device?brand=&model=`, `ir/candidates?brand=&type=`.
- `POST ir/parse`, `POST ir/test`.
- `GET config/status`, fed by `POST bundle/ack`.
- `GET ha/entities?domain=&q=` (503 when Home Assistant can't be reached), `GET ha/services?domain=`.

**New needs from this design**
- **Preview speed:** `config/preview` must answer in under 150 ms for a typical config, because it runs on every edit. If it can't, add `POST config/preview_page {config, page}`, which returns one decoded page plus its byte usage.
- **Preview on an invalid draft:** today a draft with errors returns 422 and no pages. It should return the pages that do compile (`pages[]` with a per-page `error`), so the canvas keeps rendering while the user fixes one widget.
- **Per-page byte usage** in the preview (`pages[i].bytes`, `max`, and the over-budget message), so the meters match the compiler exactly.
- **Resolved key layers:** optional. The client can merge layers itself; a `keys_resolved` field in the preview would guarantee it matches the compiler.
- **No server thumbnails needed:** thumbnails are rendered on the client from the decoded pages.
- **Live state for the palette:** `ha/entities` already includes `state`. Polling every 10 s while the Home tab is open is enough; no websocket.
- **`editor:` metadata** must be accepted by the schema and kept by import/export: who handles it, chain order, command group overrides.

## 8. Risks
- **Drag in the ingress iframe on touch:**
  - HTML5 drag and drop does not fire on touch, so pointer events are mandatory.
  - In the Home Assistant Companion app on iOS, a long press can trigger the text-selection callout; set `user-select:none` and `-webkit-touch-callout:none` on sources.
  - Vertical drags can scroll the outer HA page; `touch-action:none` on the canvas is needed.
  - Tap-source-then-tap-target is the primary path on phones.
- **Accuracy of the remote drawing:** the layout was drawn from the key list and from memory. Check positions and shapes against a photo or the bench device before release; `elite-layout.ts` makes that a data-only fix.
- **Preview latency:** the WYSIWYG feel depends on fast compiles (see section 7). Fallback: render the edited page optimistically on the client and reconcile it with the compiler's result.
- **Byte budget surprises:** a v2 page can go over budget while you edit visually. Keep the meter visible in the page inspector and on the thumbnails, and show the compiler's own fix hint.
- **Name-based guessing** (command groups, who handles it) will be wrong sometimes: always show the result and allow a per-item override.
- **Performance:** about 20 thumbnails plus a live screen. Render through Lit templates without per-frame layout; virtualise palettes over 200 items.

## 9. Open items
- Where the setup help that the apply timeout links to will live.
- Whether the Home page template should be generated automatically on first run.
