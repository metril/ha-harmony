// Pure logic of the remote studio: command grouping, action descriptions, config edits for keys and
// page widgets (grid maths, overlap, byte counter), and the assignment rules (what a drop on a
// key / cell / widget does). No DOM here, so it is all unit-tested.
import type { Config, Device, Page, Widget } from './model';

export type Action = Record<string, unknown>;
export type Layer = string; // 'default' or an activity id
export const DEFAULT_LAYER: Layer = 'default';
export const PAGE_BUDGET = 251;
export const LABEL_MAX = 24;

// ---------------------------------------------------------------- command grouping
export const GROUPS = ['power', 'volume', 'navigation', 'transport', 'numbers', 'inputs', 'other'] as const;
export type Group = (typeof GROUPS)[number];
const RE = {
  numbers: /^(?:(?:num(?:ber)?|digit|key|btn|button)[_-]?)?\d$|^(?:num|digit)s?$/,
  power: /(?:^|_)(?:power|standby|sleep|wake)(?:_|$)|^(?:on|off)$|^(?:turn_)?(?:on|off)$/,
  volume: /vol|mute|loud|bass|treble/,
  channel: /chan|^ch(?:_|$|\d)|^prev_ch|last/,
  transport: /play|pause|stop|rew|(?:^|_)ff(?:_|$)|forward|record|(?:^|_)rec(?:_|$)|skip|eject|(?:^|_)(?:next|prev|previous)(?:_|$)|backward/,
  inputs: /^input|hdmi|source|(?:^|_)(?:av|aux|tuner|optical|coax|usb|bluetooth|arc)\d?(?:_|$)|tv_audio/,
  navigation: /^(?:up|down|left|right|ok|enter|select|back|exit|menu|home|guide|info|return|nav|dpad|cursor|options|settings|tools|top_menu|popup)/,
};
/** Guess the function group of a command name; the user can see and (later) override it. */
export function groupCommand(name: string): Group {
  const n = name.toLowerCase();
  if (RE.numbers.test(n)) return 'numbers';
  if (RE.power.test(n)) return 'power';
  if (RE.volume.test(n)) return 'volume';
  if (RE.channel.test(n)) return 'navigation';
  if (RE.transport.test(n)) return 'transport';
  if (RE.inputs.test(n)) return 'inputs';
  if (RE.navigation.test(n)) return 'navigation';
  return 'other';
}

export interface Chip {
  kind: 'command' | 'input';
  device: string;
  name: string;
  group: Group;
}
/** All chips of a device, grouped. Power is built in (`power_on`/`power_off`) when the device has `power`. */
export function deviceChips(d: Device): Record<Group, Chip[]> {
  const out = Object.fromEntries(GROUPS.map((g) => [g, [] as Chip[]])) as Record<Group, Chip[]>;
  if (d.power) for (const n of ['power_on', 'power_off']) out.power.push({ kind: 'command', device: d.id, name: n, group: 'power' });
  for (const n of Object.keys(d.commands ?? {})) {
    const g = groupCommand(n);
    out[g].push({ kind: 'command', device: d.id, name: n, group: g });
  }
  for (const n of Object.keys(d.inputs ?? {})) out.inputs.push({ kind: 'input', device: d.id, name: n, group: 'inputs' });
  return out;
}

// ---------------------------------------------------------------- actions
export const isNone = (a: unknown): boolean => !!a && typeof a === 'object' && 'none' in (a as object);
export const pretty = (s: string): string => s.replace(/[_-]+/g, ' ').replace(/^./, (c) => c.toUpperCase());

/** One-line summary of a config action. */
export function describeAction(cfg: Config, a: unknown): string {
  if (!a || typeof a !== 'object') return 'Nothing';
  const x = a as Action;
  if ('device' in x) {
    const d = (cfg.devices ?? []).find((v) => v.id === x['device']);
    const dn = d?.name ?? String(x['device']);
    return 'command' in x ? `${dn} · ${x['command']}` : `${dn} · input ${x['input']}`;
  }
  if ('activity' in x) {
    if (x['activity'] === 'off') return 'All off';
    const a2 = (cfg.activities ?? []).find((v) => v.id === x['activity']);
    return `Start ${a2?.name ?? x['activity']}`;
  }
  if ('ha' in x) {
    const h = x['ha'] as { service?: string; target?: { entity_id?: string } };
    return `HA ${h.service ?? '?'} ${h.target?.entity_id ?? ''}`.trim();
  }
  if ('event' in x) return `HA event ${x['event']}`;
  if ('none' in x) return 'Does nothing';
  if ('bt' in x) return `Bluetooth ${x['bt']}`;
  return 'IR code';
}

// ---------------------------------------------------------------- sources and targets
export type Source =
  | { kind: 'command'; device: string; name: string }
  | { kind: 'input'; device: string; name: string }
  | { kind: 'activity'; id: string }
  | { kind: 'alloff' }
  | { kind: 'entity'; entity_id: string; name?: string }
  | { kind: 'ha'; service: string; entity_id: string; data?: Record<string, unknown>; repeat?: boolean }
  | { kind: 'widget'; type: 'button' | 'tile' | 'label' }
  | { kind: 'none' }
  | { kind: 'default' };
export type Target = { kind: 'key'; name: string } | { kind: 'cell'; page: number; x: number; y: number } | { kind: 'widget'; page: number; index: number };

export const sourceKey = (s: Source): string => JSON.stringify(s);
export const entityDomain = (id: string): string => id.split('.')[0] ?? '';
export const HA_ENTITY_ID = /^[a-z_]+\.[a-z0-9_]+$/;

const TOGGLE_DOMAINS = new Set(['light', 'switch', 'fan', 'cover', 'input_boolean', 'media_player', 'climate']);
const BUTTON_SERVICE: Record<string, string> = { scene: 'turn_on', script: 'turn_on', button: 'press', input_button: 'press', automation: 'trigger' };
/** Default service for an entity dropped on a key or button: toggle what toggles, run what runs. */
export function defaultService(entity_id: string): string {
  const d = entityDomain(entity_id);
  if (BUTTON_SERVICE[d]) return `${d}.${BUTTON_SERVICE[d]}`;
  if (d === 'media_player') return 'media_player.media_play_pause';
  return `${d}.${TOGGLE_DOMAINS.has(d) ? 'toggle' : 'turn_on'}`;
}
/** Entities that read better as a state tile than a button. */
export const isStateEntity = (entity_id: string): boolean => !(entityDomain(entity_id) in BUTTON_SERVICE);

/** The config action a source stands for, or undefined when it is not an action (widget types). */
export function sourceAction(s: Source): Action | undefined {
  switch (s.kind) {
    case 'command':
      return { device: s.device, command: s.name };
    case 'input':
      return { device: s.device, input: s.name };
    case 'activity':
      return { activity: s.id };
    case 'alloff':
      return { activity: 'off' };
    case 'entity':
      return { ha: { service: defaultService(s.entity_id), target: { entity_id: s.entity_id } } };
    case 'ha': {
      const ha: Action = { service: s.service, target: { entity_id: s.entity_id } };
      if (s.data && Object.keys(s.data).length) ha['data'] = s.data;
      if (s.repeat) ha['repeat'] = true;
      return { ha };
    }
    case 'none':
      return { none: true };
    default:
      return undefined;
  }
}
export function describeSource(cfg: Config, s: Source): string {
  switch (s.kind) {
    case 'command':
    case 'input':
      return `${(cfg.devices ?? []).find((d) => d.id === s.device)?.name ?? s.device} · ${s.name}`;
    case 'activity':
      return `Start ${(cfg.activities ?? []).find((a) => a.id === s.id)?.name ?? s.id}`;
    case 'alloff':
      return 'All off';
    case 'entity':
      return s.name ?? s.entity_id;
    case 'ha':
      return `${s.service} ${s.entity_id}`;
    case 'widget':
      return `a ${s.type}`;
    case 'none':
      return 'nothing';
    case 'default':
      return 'Default';
  }
}
export function sourceLabel(cfg: Config, s: Source): string {
  switch (s.kind) {
    case 'command':
    case 'input':
      return pretty(s.name);
    case 'activity':
      return (cfg.activities ?? []).find((a) => a.id === s.id)?.name ?? pretty(s.id);
    case 'alloff':
      return 'All off';
    case 'entity':
    case 'ha':
      return s.kind === 'entity' && s.name ? s.name : pretty(entityDomain(s.entity_id) === '' ? '' : (s.entity_id.split('.')[1] ?? ''));
    case 'widget':
      return pretty(s.type);
    default:
      return '';
  }
}

// ---------------------------------------------------------------- hard keys
const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;

export function layerKeys(cfg: Config, layer: Layer): Record<string, unknown> {
  if (layer === DEFAULT_LAYER) return (cfg.keys ?? {}) as Record<string, unknown>;
  return ((cfg.activities ?? []).find((a) => a.id === layer)?.keys ?? {}) as Record<string, unknown>;
}
/** New config with one hard key of one layer set; `default` removes the entry, `none` writes {none:true}. */
export function setKey(cfg: Config, layer: Layer, key: string, value: Action | 'default' | 'none'): Config {
  const next = clone(cfg);
  let holder: { keys?: Record<string, unknown> } | undefined = next;
  if (layer !== DEFAULT_LAYER) holder = (next.activities ?? []).find((a) => a.id === layer) as { keys?: Record<string, unknown> } | undefined;
  if (!holder) return next;
  if (value === 'default') {
    if (holder.keys) {
      delete holder.keys[key];
      if (layer !== DEFAULT_LAYER && !Object.keys(holder.keys).length) delete holder.keys;
    }
  } else {
    holder.keys = { ...(holder.keys ?? {}), [key]: value === 'none' ? { none: true } : value };
  }
  return next;
}

// ---------------------------------------------------------------- pages and widgets
export const isList = (p: Page): boolean => !!p.list;
export function pageDims(p: Page): { cols: number; rows: number } {
  return { cols: p.list ? (p.cols ?? 1) : (p.cols ?? 4), rows: p.list ? p.list.rows : (p.rows ?? 6) };
}
export interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}
export const boxOf = (w: Widget): Box => ({ x: w.x ?? 0, y: w.y ?? 0, w: w.w ?? 1, h: w.h ?? 1 });
export const overlaps = (a: Box, b: Box): boolean => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
/** Which config widget is dropped from the wire page when the compiler promotes a header label (v2: only without `title`). */
export function promotedIndex(p: Page): number {
  if (p.list || p.title) return -1;
  const { cols } = pageDims(p);
  return p.widgets.findIndex((w) => w.type === 'label' && !!w.label && (w.x ?? 0) === 0 && (w.y ?? 0) === 0 && (w.w ?? 1) === cols);
}
export function fits(p: Page, b: Box, ignore = -1): boolean {
  const { cols, rows } = pageDims(p);
  if (b.w < 1 || b.h < 1 || b.x < 0 || b.y < 0 || b.x + b.w > cols || b.y + b.h > rows) return false;
  return !p.widgets.some((w, i) => i !== ignore && overlaps(boxOf(w), b));
}
export function firstFree(p: Page, w = 1, h = 1, from?: { x: number; y: number }): { x: number; y: number } | null {
  const { cols, rows } = pageDims(p);
  const start = from ? from.y * cols + from.x : 0;
  for (let i = 0; i < cols * rows; i++) {
    const n = (start + i) % (cols * rows);
    const x = n % cols;
    const y = Math.floor(n / cols);
    if (fits(p, { x, y, w, h })) return { x, y };
  }
  return null;
}
export function moveBox(p: Page, index: number, dx: number, dy: number): Box | null {
  const w = p.widgets[index];
  if (!w) return null;
  const b = boxOf(w);
  const nb = { ...b, x: b.x + dx, y: b.y + dy };
  return fits(p, nb, index) ? nb : null;
}
export function resizeBox(p: Page, index: number, dw: number, dh: number): Box | null {
  const w = p.widgets[index];
  if (!w) return null;
  const b = boxOf(w);
  const nb = { ...b, w: b.w + dw, h: b.h + dh };
  return fits(p, nb, index) ? nb : null;
}
const utf8 = (s: string) => new TextEncoder().encode(s).length;
export const labelBytes = utf8;
/** Bytes a page costs by the v2 rule (header 11 + title, each widget 5 + optional bytes + label). */
export function pageBytes(p: Page): number {
  const promoted = promotedIndex(p);
  let n = 11 + utf8(p.title ?? '');
  p.widgets.forEach((w, i) => {
    if (i === promoted) return;
    const styled = (w.role && w.role !== 'default' && !w.nav) || (w.shape && w.shape !== 'card') || w.icon_only;
    n += 5 + (styled || w.nav ? 1 : 0) + (w.icon && w.icon !== 'none' ? 1 : 0) + (w.nav || (w['bind'] as unknown) ? 1 : 0) + utf8(w.label ?? '');
  });
  return n;
}
/** How many label bytes widget `index` may still use: the 24 B limit and what is left of the page budget. */
export function labelRoom(p: Page, index: number, budget = PAGE_BUDGET): number {
  const w = p.widgets[index];
  if (!w) return LABEL_MAX;
  const others = pageBytes(p) - utf8(w.label ?? '');
  return Math.max(0, Math.min(LABEL_MAX, budget - others));
}
export function trimToBytes(s: string, max: number): string {
  let out = '';
  for (const ch of s) {
    if (utf8(out + ch) > max) break;
    out += ch;
  }
  return out;
}

export function uniqueWidgetId(cfg: Config, base: string): string {
  const used = new Set((cfg.pages ?? []).flatMap((pg) => pg.widgets.map((w) => w.id)));
  let n = 1;
  while (used.has(`${base}${n}`)) n++;
  return `${base}${n}`;
}

export function setWidget(cfg: Config, page: number, index: number, patch: Record<string, unknown>): Config {
  const next = clone(cfg);
  const w = next.pages?.[page]?.widgets[index];
  if (!w) return next;
  for (const [k, v] of Object.entries(patch)) {
    if (v === undefined || v === null || v === '') delete w[k];
    else w[k] = v;
  }
  return next;
}
export function removeWidget(cfg: Config, page: number, index: number): Config {
  const next = clone(cfg);
  next.pages?.[page]?.widgets.splice(index, 1);
  return next;
}
export function placeWidget(cfg: Config, page: number, index: number, b: Box): Config {
  return setWidget(cfg, page, index, { x: b.x, y: b.y, w: b.w, h: b.h });
}

export interface Dropped {
  config: Config;
  /** announcement + toast text */
  message: string;
  /** widget created or changed (select it) */
  widget?: { page: number; index: number };
  key?: string;
}
export type DropRefusal = { refused: string };
const isRefusal = (r: Dropped | DropRefusal): r is DropRefusal => 'refused' in r;
export { isRefusal };

/** Build the widget a source makes on an empty cell. */
function widgetFor(cfg: Config, page: Page, s: Source, at: { x: number; y: number }): { widget: Widget } | DropRefusal {
  const { cols } = pageDims(page);
  const wide = Math.min(2, cols);
  const base = { x: at.x, y: at.y };
  const sized = page.list ? {} : { ...base };
  if (s.kind === 'widget') {
    if (s.type === 'label') return { widget: { type: 'label', ...sized, w: Math.min(cols, 2), label: 'Label' } };
    if (s.type === 'tile') return { widget: { type: 'tile', id: uniqueWidgetId(cfg, 'tile'), ...sized, w: wide, h: 1, label: 'Tile', entity: 'sensor.choose_entity' } };
    return { widget: { type: 'button', id: uniqueWidgetId(cfg, 'btn'), ...sized, w: wide, h: 1, label: 'Button', action: { none: true } } };
  }
  if (s.kind === 'entity' && isStateEntity(s.entity_id)) {
    return { widget: { type: 'tile', id: uniqueWidgetId(cfg, 'tile'), ...sized, w: wide, h: 1, label: trimToBytes(s.name ?? pretty(s.entity_id.split('.')[1] ?? ''), LABEL_MAX), entity: s.entity_id } };
  }
  const action = sourceAction(s);
  if (!action) return { refused: 'That cannot be placed on the screen.' };
  return { widget: { type: 'button', id: uniqueWidgetId(cfg, 'btn'), ...sized, w: wide, h: 1, label: trimToBytes(sourceLabel(cfg, s), LABEL_MAX), action } };
}

/**
 * What dropping `s` on `t` does to the config (the one function behind drag, click-click and keyboard).
 * `haOk` is false when the backend has no Home Assistant actions: those drops are refused with the reason.
 */
export function assign(cfg: Config, layer: Layer, s: Source, t: Target, haOk = true): Dropped | DropRefusal {
  const needsHa = s.kind === 'entity' || s.kind === 'ha';
  if (t.kind === 'key') {
    if (s.kind === 'widget') return { refused: 'Widgets go on the screen, not on a key.' };
    if (needsHa && !haOk) return { refused: 'This add-on version cannot store Home Assistant actions yet.' };
    const verb = layer === DEFAULT_LAYER ? 'Default' : 'this layer';
    if (s.kind === 'default' || s.kind === 'none') {
      const config = setKey(cfg, layer, t.name, s.kind);
      return { config, key: t.name, message: s.kind === 'default' ? `${t.name} now uses Default` : `${t.name} does nothing in ${verb}` };
    }
    const action = sourceAction(s) as Action;
    return { config: setKey(cfg, layer, t.name, action), key: t.name, message: `Assigned ${describeSource(cfg, s)} to ${t.name}${layer === DEFAULT_LAYER ? '' : ', overriding Default'}` };
  }
  const page = cfg.pages?.[t.page];
  if (!page) return { refused: 'There is no page to put that on.' };
  if (needsHa && !haOk && !(s.kind === 'entity' && isStateEntity(s.entity_id))) return { refused: 'This add-on version cannot store Home Assistant actions yet.' };
  if (t.kind === 'cell') {
    let at = { x: t.x, y: t.y };
    const bigger = s.kind === 'widget' && s.type === 'label' ? Math.min(2, pageDims(page).cols) : Math.min(2, pageDims(page).cols);
    if (!page.list && !fits(page, { ...at, w: 1, h: 1 })) {
      const free = firstFree(page, 1, 1, at);
      if (!free) return { refused: 'This page is full.' };
      at = free;
    }
    const made = widgetFor(cfg, page, s, at);
    if ('refused' in made) return made;
    const w = made.widget;
    if (!page.list) {
      // shrink to what fits at the spot
      let width = Math.min(w.w ?? 1, bigger);
      while (width > 1 && !fits(page, { x: at.x, y: at.y, w: width, h: w.h ?? 1 })) width--;
      w.w = width;
    }
    const next = clone(cfg);
    const pg = next.pages?.[t.page] as Page;
    pg.widgets.push(w);
    return { config: next, widget: { page: t.page, index: pg.widgets.length - 1 }, message: `Added ${w.type} ${w.label ?? ''} to page ${page.title || page.id} at column ${(w.x ?? 0) + 1}, row ${(w.y ?? 0) + 1}` };
  }
  const w = page.widgets[t.index];
  if (!w) return { refused: 'That widget is gone.' };
  if (s.kind === 'widget') return { refused: 'Drop a widget type on an empty cell.' };
  if (w.type === 'tile') {
    if (s.kind !== 'entity') return { refused: 'A tile shows one Home Assistant entity: drop an entity on it.' };
    return { config: setWidget(cfg, t.page, t.index, { entity: s.entity_id }), widget: { page: t.page, index: t.index }, message: `Tile now shows ${s.entity_id}` };
  }
  if (w.type === 'label') return { refused: 'A label has no action.' };
  const action = sourceAction(s);
  if (!action) return { refused: 'That is not an action.' };
  const cleaned = setWidget(cfg, t.page, t.index, { action, nav: undefined });
  return { config: cleaned, widget: { page: t.page, index: t.index }, message: `Button ${w.label ?? ''} now does ${describeSource(cfg, s)}` };
}

// ---------------------------------------------------------------- layers and pages
export function pageIndexForLayer(cfg: Config, layer: Layer): number {
  const pages = cfg.pages ?? [];
  if (layer !== DEFAULT_LAYER) {
    const i = pages.findIndex((p) => (p as Page & { activity?: string }).activity === layer);
    if (i >= 0) return i;
  }
  const home = pages.findIndex((p) => p.kind === 'home');
  return home >= 0 ? home : 0;
}
export const activityIndex = (cfg: Config, layer: Layer): number | null => {
  const i = (cfg.activities ?? []).findIndex((a) => a.id === layer);
  return i >= 0 ? i : null;
};

export function uniquePageId(cfg: Config, base: string): string {
  const used = new Set((cfg.pages ?? []).map((p) => p.id));
  if (!used.has(base)) return base;
  let n = 2;
  while (used.has(`${base}_${n}`)) n++;
  return `${base}_${n}`;
}
export function movePage(cfg: Config, from: number, to: number): Config {
  const next = clone(cfg);
  const pages = next.pages ?? [];
  if (from < 0 || to < 0 || from >= pages.length || to >= pages.length) return next;
  const [p] = pages.splice(from, 1);
  pages.splice(to, 0, p as Page);
  return next;
}
export function duplicatePage(cfg: Config, index: number): Config {
  const next = clone(cfg);
  const src = next.pages?.[index];
  if (!src) return next;
  const copy = clone(src) as Page;
  copy.id = uniquePageId(next, src.id + '_copy');
  delete copy.kind; // at most one home / favorites page
  delete (copy as Page & { activity?: string }).activity;
  const used = new Set<string>();
  (next.pages ?? []).forEach((pg) => pg.widgets.forEach((w) => w.id && used.add(w.id)));
  copy.widgets.forEach((w) => {
    if (!w.id) return;
    let n = 2;
    while (used.has(`${w.id}${n}`)) n++;
    w.id = `${w.id}${n}`;
    used.add(w.id);
  });
  next.pages?.splice(index + 1, 0, copy);
  return next;
}
export function deletePage(cfg: Config, index: number): Config {
  const next = clone(cfg);
  next.pages?.splice(index, 1);
  return next;
}

export type TemplateId = 'activities' | 'activity' | 'numbers' | 'lights' | 'devices' | 'blank';
export const TEMPLATES: { id: TemplateId; name: string; note: string }[] = [
  { id: 'activities', name: 'Activities home', note: 'One button per activity, plus All off' },
  { id: 'activity', name: 'Activity control', note: 'Transport and volume of the first device' },
  { id: 'numbers', name: 'Number pad', note: '0-9 of the first device that has them' },
  { id: 'lights', name: 'Lights and scenes', note: 'Tiles for Home Assistant lights' },
  { id: 'devices', name: 'Device list', note: 'One row per device command' },
  { id: 'blank', name: 'Blank', note: 'Empty 4x6 grid' },
];
export function templatePage(cfg: Config, id: TemplateId, ctx: { entities?: { entity_id: string; name?: string }[]; activity?: string } = {}): Page {
  const devices = cfg.devices ?? [];
  const pid = uniquePageId(cfg, id === 'activities' ? 'home' : id);
  const used = new Set((cfg.pages ?? []).flatMap((pg) => pg.widgets.map((w) => w.id)));
  const mk = (base: string) => {
    let n = 1;
    while (used.has(`${base}${n}`)) n++;
    used.add(`${base}${n}`);
    return `${base}${n}`;
  };
  const btn = (x: number, y: number, label: string, action: Action, extra: Partial<Widget> = {}, w = 2): Widget => ({ type: 'button', id: mk('btn'), x, y, w, h: 1, label: trimToBytes(label, LABEL_MAX), action, ...extra });
  switch (id) {
    case 'activities': {
      const acts = (cfg.activities ?? []).slice(0, 5);
      const widgets = acts.map((a, i) => btn(0, i, a.name ?? a.id, { activity: a.id }, {}, 4));
      widgets.push(btn(0, Math.min(acts.length, 5), 'All off', { activity: 'off' }, { role: 'danger' }, 4));
      const hasHome = (cfg.pages ?? []).some((p) => p.kind === 'home');
      return { id: pid, cols: 4, rows: 6, ...(hasHome ? {} : { kind: 'home' as const }), title: 'Activities', widgets };
    }
    case 'activity': {
      const d = devices[0];
      const names = new Set(Object.keys(d?.commands ?? {}));
      const pick = (...c: string[]) => c.find((n) => names.has(n));
      const slots: [string, string | undefined][] = [['Vol +', pick('vol_up', 'volume_up')], ['Vol -', pick('vol_down', 'volume_down')], ['Mute', pick('mute')], ['Play', pick('play', 'play_pause')], ['Pause', pick('pause')], ['Stop', pick('stop')]];
      const widgets: Widget[] = [];
      slots.forEach(([label, cmd]) => {
        if (cmd && d && widgets.length < 6) widgets.push(btn((widgets.length % 2) * 2, Math.floor(widgets.length / 2), label, { device: d.id, command: cmd }));
      });
      return { id: pid, cols: 4, rows: 6, title: d?.name ?? 'Control', ...(ctx.activity ? { activity: ctx.activity } : {}), back: true, widgets };
    }
    case 'numbers': {
      const d = devices.find((dv) => Object.keys(dv.commands ?? {}).some((n) => groupCommand(n) === 'numbers')) ?? devices[0];
      const widgets: Widget[] = [];
      for (let n = 1; n <= 10 && d; n++) {
        const c = n % 10;
        const cmd = Object.keys(d.commands ?? {}).find((k) => RE.numbers.test(k.toLowerCase()) && k.endsWith(String(c)));
        if (cmd) widgets.push(btn(c === 0 ? 1 : (n - 1) % 3, c === 0 ? 3 : Math.floor((n - 1) / 3), String(c), { device: d.id, command: cmd }, { shape: 'pill' }, 1));
      }
      return { id: pid, cols: 3, rows: 4, title: 'Numbers', back: true, widgets };
    }
    case 'lights': {
      const lights = (ctx.entities ?? []).filter((e) => entityDomain(e.entity_id) === 'light').slice(0, 8);
      const widgets: Widget[] = lights.map((e, i) => ({ type: 'tile', id: mk('tile'), x: (i % 2) * 2, y: Math.floor(i / 2), w: 2, h: 1, label: trimToBytes(e.name ?? e.entity_id, LABEL_MAX), entity: e.entity_id, icon: 'bulb' }));
      return { id: pid, cols: 4, rows: 6, title: 'Lights', back: true, widgets };
    }
    case 'devices': {
      const widgets: Widget[] = [];
      devices.forEach((d) => Object.keys(d.commands ?? {}).slice(0, 12).forEach((c) => widgets.length < 30 && widgets.push({ type: 'button', id: mk('btn'), label: trimToBytes(`${d.name ?? d.id} ${c}`, LABEL_MAX), action: { device: d.id, command: c } })));
      return { id: pid, cols: 1, list: { rows: 6 }, title: 'Devices', back: true, widgets };
    }
    default:
      return { id: pid, cols: 4, rows: 6, title: 'New page', widgets: [] };
  }
}
export function addPage(cfg: Config, page: Page, at?: number): Config {
  const next = clone(cfg);
  next.pages = next.pages ?? [];
  next.pages.splice(at ?? next.pages.length, 0, page);
  return next;
}
export function setPage(cfg: Config, index: number, patch: Record<string, unknown>): Config {
  const next = clone(cfg);
  const p = next.pages?.[index] as (Page & Record<string, unknown>) | undefined;
  if (!p) return next;
  for (const [k, v] of Object.entries(patch)) {
    if (v === undefined || v === null || v === '' || v === false) delete p[k];
    else p[k] = v;
  }
  return next;
}

// ---------------------------------------------------------------- errors -> targets
export type ErrorTarget = { kind: 'key'; layer: Layer; key: string } | { kind: 'widget'; page: number; index: number; field?: string } | { kind: 'page'; page: number } | null;
/** Map a validation path to the thing on the canvas it points at. */
export function errorTarget(path: (string | number)[]): ErrorTarget {
  const [a, b, c, d] = path;
  if (a === 'pages' && typeof b === 'number') {
    if (c === 'widgets' && typeof d === 'number') return { kind: 'widget', page: b, index: d, field: String(path[4] ?? '') };
    return { kind: 'page', page: b };
  }
  if (a === 'keys' && typeof b === 'string') return { kind: 'key', layer: DEFAULT_LAYER, key: b };
  if (a === 'activities' && c === 'keys' && typeof d === 'string') return { kind: 'key', layer: `#${String(b)}`, key: d };
  return null;
}
