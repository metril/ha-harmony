# Harmony Editor: web UI spec

The UX and visual spec for the add-on's editor (`harmony_addon`), served in the Home Assistant ingress
iframe. Mockups: [editor-ui-mockups.html](editor-ui-mockups.html). The API is `harmony_addon/app.py`,
plus the additions listed in section 6.

Items marked **[decided 2026-11-18, revisit on request]** were decided while the user was away.

## 1. Navigation and layout
- **Header (48 px):** title, hub picker, Import YAML, Export YAML.
  - The hub picker is shown only when `GET api/v1/hubs` returns more than one hub. **[decided 2026-11-18, revisit on request]**
  - The hub is a `?hub=` value that is part of the URL.
- **Left rail (216 px):** Overview · Devices · Activities · Hard keys · Remote pages · (divider) IR library · History.
  Each entry shows a count or a red error badge.
- **Draft bar (sticky, top of the content area):**
  - State dot + "Draft · N changes not applied" + "saved 3 s ago", or "No changes since rev N".
  - A chip reading "Valid" or "N errors · Show" (Show jumps to the first error).
  - Meters: config KiB, IR pool %, commands /256. A meter turns amber at 80% and red at 100%.
  - Buttons: Discard draft (ghost, undo icon) and Review & apply (primary).
  - When there are errors, Apply is disabled and its tooltip says why ("Fix 1 error to apply").
- **Phone (about 400 px):**
  - The rail becomes a menu button in the top bar.
  - The draft bar becomes a bottom strip with the change count, error count and Apply.
  - Touch targets are at least 44 px.
- **First run** (no draft and nothing applied): a centred empty state with "Import YAML file" and "Add first device".
- **Autosave:**
  - Edits PUT the whole draft about 600 ms after the last change.
  - The request carries `If-Match: <rev>`.
  - A 409 conflict shows a "Changed in another tab: Reload / Overwrite" banner.

## 2. Screens
### Overview
- Stat cards: devices /16, activities /16, page widgets /128, inputs /64.
- An activities list.
- "Changes in this draft": a client-side diff of the draft against the applied config, as Added/Changed/Removed rows.
- The error banner has "Go to field".
- **Permissions panel:**
  - Lists every entity and service the draft will ask Home Assistant to control. It is collected from all Home Assistant actions and tile entities.
  - Note: "After Apply, Home Assistant asks you to approve changes to this list."

### Devices
- Master-detail: a filterable device list (240 px) beside the editor.
- Editor header: name, an id chip, a source chip ("From IR archive · …"), Delete.
- Tabs: Commands / Power / Inputs / Settings (aliases, settle_ms).
- **Command table:**
  - Columns: checkbox, name (inline input, 31-byte counter), code summary in mono, IR ports (1/2/3 toggles), "Used by" (reverse references), Send IR, and a ⋯ menu (edit code, duplicate, delete).
  - Selecting rows swaps the toolbar for a bulk bar: Set IR ports… / Rename… / Delete N / Clear.
- **Code editor** (in a dialog): kind tabs for Protocol (proto/addr/cmd with per-protocol ranges), Pronto, Raw, Flipper file, SmartIR file, From archive.
  - Pasted and uploaded text goes through `POST api/v1/ir/parse`.
- **Power:** a segmented choice of Separate on/off, Toggle, or None. Each code field has its own Send IR.
- **Inputs:** the same table format as commands. Names use the reserved `input_` prefix, which the UI hides.
- Deleting something that is used anywhere opens a confirm dialog listing what uses it.

### Send IR (one component everywhere)
- Amber pill with an IR-wave icon and the label "Send IR". It is the only amber control in the app.
- After sending: "Sent" with a check, then a countdown ("wait 1 s"). The server enforces the rate limit through `POST api/v1/ir/test`.
- Works on draft codes: the add-on sends the code once through the hub. **[decided 2026-11-18, revisit on request]**
- Disabled while the field is invalid.
- Never fires on hover, row click, or Enter inside a text field.

### Add-device wizard
Full page with a stepper: Brand → Type → Power test → Model → Review & add.
- **Brand** (`ir/brands?q=`): a search box with a virtualised result list of name and model count; a recents list; type-ahead with keyboard navigation.
- **Type:** cards (TV, AVR, …).
- **Power test** (`ir/candidates?brand=&type=`):
  - Shows "Code 2 of 7", the kind (on/off/toggle), the code in mono, "Used by N models", and example models.
  - A large Send power code button (56 px), a blaster port select, and a status line ("Sent at …, next send in 2 s").
  - The question "Did the TV turn on or off?" with: Yes, it reacted · No, try the next code · Send again. Shortcuts: Space / Y / N.
  - A "Tried" list on the side, and "Skip the test: search N models".
- **Model** (`ir/models?brand=&q=`): a virtualised list filtered to the codesets that matched.
- **Review** (`ir/device?brand=&model=`):
  - The command list with checkboxes and per-command Send IR, and a rename field.
  - Add device writes a draft only; nothing reaches the hub until Apply.
- **Offline:** a red banner ("The IR archive can't be reached. Cached brands still work. Retry"). Archive errors are listed per item and are not fatal.
- Other sources (Paste Pronto, upload a Flipper or SmartIR file, and later learning from the original remote) open as separate entry points.

### Activity editor
- **Devices and inputs:** one row per device with an input select (or "No input switch") and remove; shows N/8.
- **Sequence:** a Start/Stop segmented control.
  - Generated steps are shown locked and grey: power on all devices, set inputs, wait for settle (the largest settle_ms).
  - Then the user's own steps: drag handle plus up/down/remove buttons; Alt+↑/↓ reorders from the keyboard; shows N/16.
- **Who handles it:**
  - Pickers for Volume, Channels, Navigation and Transport.
  - Each fills the activity's `keys` by matching command names, and reports keys that found no match with a link to Hard keys.
  - The choices are stored in a top-level `editor:` metadata mapping that the compiler ignores, so they survive reload and YAML export. **[decided 2026-11-18, revisit on request]**

### Hard keys
- Layer segmented control: Default | each activity.
- Three columns:
  - **SVG schematic of the remote:** 35 keys as focusable `role=button` shapes with an `aria-label` such as "Select: Streaming box · ok, override".
    - Override = filled accent, inherited = light, does nothing = dashed.
    - The schematic stays; tracing it from a photo is a later polish item. **[decided 2026-11-18, revisit on request]**
  - **Key list:** filterable, grouped by function.
  - **Inspector:** "Uses Default / Does something else / Does nothing", action picker, Send IR, "Use Default instead".
- The Default layer hides "Uses Default".

### Action picker (shared)
- **Kind select:** Device command / Device input / Start activity / All off / Home Assistant action / HA event / Bluetooth / Raw IR / Nothing. Kinds that are not allowed in the current context (no activity in steps/off) are filtered out.
- After the kind, cascading selects pick the details.
- **Home Assistant action** **[decided 2026-11-18, revisit on request]:**
  - **Service:** `domain.service`, from an allow-list of domains: light, switch, scene, script, media_player, remote, fan, cover, climate, input_boolean, input_button, button, number, select.
  - **Target entity id(s):**
    - Typed as free text, checked against `^[a-z_]+\.[a-z0-9_]+$`, with the domain shown as you type.
    - Inline note: "The add-on can't read your Home Assistant entity list, so type or paste entity ids." Paste/import of an entity list comes later.
  - **Data:** optional key/value rows.
  - **Keys only:** a "Repeat while held" toggle.
  - Tiles get the same "When tapped" Home Assistant action.

### Page editor
**Layout**
- **Page list:** drag plus Alt+↑/↓ reorder, the page's id number shown, per-page bytes and widget count, an amber chip at 85% full or more.
- **Page settings (v2):**
  - Kind: Normal / Home / Favorites.
  - Title (24-byte counter).
  - Activity (the page auto-shows when it starts).
  - Back (shows a back chevron; Exit goes Home).
  - List mode (rows: N). The list shows the paged group as "1/3, 2/3…" and the canvas shows the remote's "idx/cnt" in its bar.

**Canvas**
- Drawn from preview `pages[]` at 1×/2×/3× in the remote's own palette, with a faint cell grid.
- Pointer: drag to move, corner handles to resize, snapped to cells, with overlaps refused live.
- Keyboard: Tab between widgets; arrows move; Shift+arrows resize; Del removes; N adds a widget at the first free cell (shown as a ghost cell).
- The canvas is `role=application` with a live region that reads out the position.

**Byte meter**
- Follows the v2 budget rule in `docs/plans/2026-11-18-page-format-v2.md`:
  - header 11 + title bytes;
  - each widget 5 + its optional bytes (style/icon/bind, 1 each, dropped when they hold the default) + label bytes;
  - at most 251 bytes per page.
- Over the budget, the meter shows the compiler's own message ("N bytes over; shorten labels, use icon only, or make it a list").
- Widget meters: /32 per page here, /128 across all pages in the draft bar.

**Inspector**
- Type control (Button/Tile/Label).
- Label with a live UTF-8 byte counter (n/24), red when over the limit, with a hint that accents and emoji take 2–4 bytes.
- Id with its derived `tap_<id>` event name.
- Col/Row/W/H number fields.
- "When tapped": the action picker for buttons and tiles; tiles also take an entity.
- **Look (live, v2):**
  - Role: default/primary/secondary/danger.
  - Shape: card/row/pill/monogram.
  - Icon picker: the 44 stock icons from the shared `icons.json`, as a searchable grid plus "none".
  - Icon only (the label is kept for the editor but not drawn).
  - Nav target: next / prev / home / a page. A nav target replaces the tap action.

**Phone:** canvas at 1.25×, inspector as a bottom sheet with arrow buttons for moving.

### Apply
- **Dialog:**
  - Title "Apply draft to <hub>?", the diff list, the three meters, a note about running activities, the config hash.
  - Buttons: Cancel / Apply to hub. Focus is trapped; Esc cancels.
- **Progress** (in the same dialog, `aria-live`): Validated → Saved as rev N → Waiting for Home Assistant → Pushed to hub → Remote pages updated.
  - The UI polls `GET api/v1/config/status?hub=`, which the integration's `POST api/v1/bundle/ack` updates. **[decided 2026-11-18, revisit on request]**
- **No acknowledgement within 30 s:**
  - The dialog says: "Saved as rev N. Home Assistant hasn't picked it up yet. Is the Harmony integration set up and linked to this add-on?"
  - It links to setup help and closes with "Keep waiting in background".
- **422 response:** the dialog closes and jumps to the errors.
- **Ack with `error`:** a red banner shows the detail, says the hub keeps the old rev, and offers "Revert to rev N".

### History
- Lists rev, time and hash for the last 5.
- Actions: View diff, Restore as draft, Revert to previous. Revert needs a confirm.

### Import/Export
- Import uploads a file with the YAML content type and asks first if a draft exists ("Replace draft?").
- If the YAML refers to IR files, a 422 `ir_files_unresolvable` lists them, each with "Upload this file" (through `ir/parse`).
- Export downloads the draft or applied config with `?source=`.

### Validation errors
- The server returns all errors as a list.
- Each `{path, msg}` is turned into a route plus field id and rendered three ways:
  - next to the field (red outline + message, `aria-describedby`);
  - as a count on the rail badge;
  - in the draft-bar chip.
- Errors with no path go in a banner.
- Server messages are always rendered as text, never as HTML.

## 3. Design system
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

- `ir` is used only for Send IR and the draft state.
- Dark tokens apply under `@media (prefers-color-scheme: dark)` with `:root:not([data-theme="light"])`, and under `[data-theme="dark"]`.
- **Remote canvas palette** (the remote's own direction-B colours, same in both themes): #101820 / 182430 / 202C38 / 304050, text #F8FCF8 / A8B4C0, accent #20C8B0, warm #F8C040, bad #F85848.

**Type**
- Font: system-ui; codes in ui-monospace.
- Sizes 12 / 13 / 14 (base, 1.45 line height) / 16 / 20 / 24. Weights 400 / 500 / 600 / 650.

**Spacing and shape**
- 4 px base: 4, 8, 12, 16, 20, 24, 32.
- Radii 6 / 10 / 16.
- Controls 36 px tall (28 small); 44 px touch targets on the phone.

**Components**
- Button: default / primary / ghost / danger / icon; states hover, active, focus, and disabled with a reason.
- Send IR pill: idle / sending / cooldown / disabled.
- Input: default / focus / error / with byte counter.
- Also: select or combobox, segmented control, tabs, chip, meter, table (row / selected / error), list row (default / locked / dragging), stepper, dialog, banner, toast, empty state.

**Icons**
- One inline SVG sprite on a 24-unit grid, 1.8 stroke, `currentColor`.
- Page-widget icons come from the shared `icons.json`.
- Icons always come with a text label, except ⋯ / × / arrow buttons, which get an `aria-label`.

**Focus and accessibility**
- Focus ring: 2 px accent with a 2 px offset.
- Everything works by keyboard; skip link to content.
- Landmarks: nav, main, and the draft bar as a region.
- `aria-live` for save status and progress.
- Contrast at least 4.5:1.

**Motion**
- 120–180 ms ease-out for state changes only.
- None when the user prefers reduced motion. No decorative animation.

**Copy**
- Buttons say what they do: "Apply to hub", "Send IR", "Discard draft".
- The words "hub", "remote" and "IR" are used consistently.
- Error messages say what to change ("Shorten the label by 2 bytes").
- Plain sentence case throughout.

## 4. Technical constraints
**Build**
- Lit 3 + TypeScript + Vite, built to a single JS file and a single CSS file served by the add-on.

**CSP**
- The current CSP (`style-src 'self'; script-src 'self'`) is enough as it is.
- Component styles use Lit `static styles`; adopted constructable stylesheets are not blocked by the CSP.
- Global tokens go in one bundled `app.css` linked from the page.
- Positions are set only through `styleMap` (which uses `setProperty`).
- Never bind a `style="${…}"` attribute, `<style>` tags, `innerHTML` or `unsafeHTML`.

**`api.ts`**
- Builds URLs relative to `document.baseURI`, with no leading slash (for example `api/v1/config?hub=`).
- Sets JSON content type and `X-HH-CSRF: 1` on every mutating request; the YAML import uses the YAML content type.
- Uses a single queue for draft PUTs, each with `If-Match`.
- Edits are applied in the UI immediately and rolled back with a toast if the request fails.

**Other**
- Long lists use `@lit-labs/virtualizer`, bundled locally, not from a CDN.
- No fonts, images or requests to external hosts.

## 5. Build order
Tasks 1–3 already give a usable editor for devices plus Apply.

1. Scaffold: Vite + Lit, CSP-safe styles, `api.ts`, shell (rail, hub picker, router), draft store with `If-Match` and conflict banner, draft bar with Apply dialog and status polling.
2. Devices: list, command table, code editor (proto / pronto / raw), power, inputs, settings, mapping every error to its field.
3. Import/export, history, revert, first-run empty state.
4. Send IR component (`ir/test`).
5. Action picker, including the Home Assistant action, the reference index ("Used by", delete guards) and the Permissions panel.
6. Activities: devices and inputs, start/stop sequence, "Who handles it" (`editor:` metadata).
7. Hard keys: SVG remote, layers, inspector.
8. Page editor: page list, preview canvas, inspector, byte meters.
9. Page format v2 in the editor: Look section, icon picker, page settings, list mode and paged groups, v2 byte rule. Needs page format v2.
10. Page editor pointer and keyboard move/resize, add widget at the first free cell.
11. Add-device wizard: brand/type/model search (`ir/brands`, `ir/models`, `ir/device`).
12. Candidate power-test loop (`ir/candidates`; needs 4 and 11).
13. Flipper/SmartIR/Pronto file import into commands (`ir/parse`).
14. Learning from the original remote (needs a learn endpoint).

## 6. API the frontend relies on
**Existing**
- `GET config`, `PUT config/draft`, `POST config/{validate,preview,apply,revert,import_yaml}`, `GET config/export_yaml`, `GET health`.

**Being built** **[decided 2026-11-18, revisit on request]**
- `If-Match`/rev on draft PUT, with 409 on conflict.
- Validation returns all errors as a list.
- `GET hubs`.
- `GET ir/brands?q=`, `ir/models?brand=&q=`, `ir/device?brand=&model=`, `ir/candidates?brand=&type=`.
- `POST ir/parse`, `POST ir/test` (rate-limited).
- `GET config/status?hub=`, fed by the integration's `POST bundle/ack {hub, rev, result: ok|error, detail}` (bearer token).

## 7. Open items
- Where the setup help that the apply timeout links to will live.
- The exact wording of the Home Assistant approval step: it depends on how the integration asks the user.

## Amendment 9 (user decision, 2026-11-18): the add-on may read Home Assistant

The add-on now has Home Assistant API access (`homeassistant_api: true`). This supersedes the
"entity ids are typed, the add-on cannot read the entity list" parts of amendment 6:

- Entity pickers (tiles, Home Assistant actions) search the real entity list:
  `GET api/v1/ha/entities?domain=&q=` → `[{entity_id, name, domain, area?, state?, device_class?}]`.
  Show name + entity id + area; free-text entry stays as the fallback when the route answers 503.
- The service picker for Home Assistant actions uses `GET api/v1/ha/services?domain=` (service names
  and fields) for the allow-listed domains; data fields render from the service's field list.
- The Overview "Permissions" panel becomes informational: it lists the entities and services the
  draft uses. There is no approval step in Home Assistant.
