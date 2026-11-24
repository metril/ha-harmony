// Pure logic of the remote studio: command grouping, action descriptions, config edits for keys and
// page widgets (grid maths, overlap, byte counter), and the assignment rules (what a drop on a
// key / cell / widget does). No DOM here, so it is all unit-tested.
import { commandNames } from './bt';
import type { Config, Device, IrAction, Page, Widget } from './model';
import { tileDefaultTap } from './permissions';
import { colourBytes } from './theme-model';
import { bestIcon } from './icon-suggest';
import { abbreviate, displayName, dn, fitName } from './key-names';
import { keyTitle } from './remote-layout';
import { stockIconByName, stockIconId } from './stock-icons';
import type { PreviewPage } from './api';
import { labelBudget, textPx, type FitWidget, type Font } from './text-fit';

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
  // a command that cycles / toggles the source is not an input of its own (only the device's `inputs` are)
  cycles: /(?:source|input|src|inp)s?[_-]?(?:toggle|cycle|next|prev|previous|switch|select)|(?:toggle|cycle|next|prev|previous|switch)[_-]?(?:source|input|src|inp)/,
  inputs: /^input|hdmi|source|(?:^|_)(?:av|aux|tuner|optical|coax|usb|bluetooth|arc)\d?(?:_|$)|tv_audio/,
  navigation: /^(?:up|down|left|right|ok|enter|select|back|exit|menu|home|guide|info|return|nav|dpad|cursor|options|settings|tools|top_menu|popup)/,
};
/** Guess the function group of a command name; the user can see and (later) override it. */
export function groupCommand(name: string): Group {
  const n = name.toLowerCase();
  if (RE.numbers.test(n)) return 'numbers';
  if (RE.power.test(n)) return 'power';
  if (RE.cycles.test(n)) return 'other';
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
  for (const n of commandNames(d)) {
    const g = groupCommand(n);
    out[g].push({ kind: 'command', device: d.id, name: n, group: g });
  }
  for (const n of Object.keys(d.inputs ?? {})) out.inputs.push({ kind: 'input', device: d.id, name: n, group: 'inputs' });
  return out;
}

// ---------------------------------------------------------------- actions
export const isNone = (a: unknown): boolean => !!a && typeof a === 'object' && 'none' in (a as object);
import { displayCommand } from './key-names';
export const pretty = (s: string): string => s.replace(/[_-]+/g, ' ').replace(/^./, (c) => c.toUpperCase());

/** One-line summary of a config action. */
export function describeAction(cfg: Config, a: unknown, names?: ReadonlyMap<string, string>): string {
  if (!a || typeof a !== 'object') return 'Nothing';
  const x = a as Action;
  if ('device' in x) {
    const d = (cfg.devices ?? []).find((v) => v.id === x['device']);
    const dn = d?.name ?? String(x['device']);
    return 'command' in x ? `${dn} · ${displayCommand(String(x['command']))}` : `${dn} · input ${displayCommand(String(x['input']))}`;
  }
  if ('activity' in x) {
    if (x['activity'] === 'off') return 'All Off';
    const a2 = (cfg.activities ?? []).find((v) => v.id === x['activity']);
    return `Start ${a2?.name ?? x['activity']}`;
  }
  if ('ha' in x) {
    const h = x['ha'] as { service?: string; target?: { entity_id?: string | string[] }; repeat?: boolean };
    const ids = h.target?.entity_id;
    const list = Array.isArray(ids) ? ids : ids ? [ids] : [];
    return haLabel(list[0] ?? '', Math.max(0, list.length - 1), h.service ?? '?', !!h.repeat, names);
  }
  if ('event' in x) return `HA event ${x['event']}`;
  if ('none' in x) return 'Does nothing';
  if ('bt' in x) return `Bluetooth ${x['bt']}`;
  return 'IR code';
}

export const REPEAT_MARK = '\u21BB';
export const LABEL_CLIP = 26;
/** "Lamp · toggle": the entity's friendly name (or id) and the short form of the service; ↻ when it repeats while held. */
export function haLabel(first: string, more: number, service: string, repeat: boolean, names?: ReadonlyMap<string, string>): string {
  const who = (names?.get(first) || first || 'Home Assistant') + (more > 0 ? ` +${more}` : '');
  return `${who} \u00B7 ${service.split('.').pop() || service}${repeat ? ` ${REPEAT_MARK}` : ''}`;
}
/** True when the action runs through Home Assistant (an `ha:` action, or a command / input of a `kind: ha` device): no IR to test-fire. */
export function isHaAction(cfg: Config, a: unknown): boolean {
  if (!a || typeof a !== 'object') return false;
  const x = a as Action;
  if ('ha' in x) return true;
  return 'device' in x && (cfg.devices ?? []).find((v) => v.id === x['device'])?.kind === 'ha';
}
/** `s` cut to `max` characters with an ellipsis (callers keep the full text in a title). */
export const clip = (s: string, max = LABEL_CLIP): string => (Array.from(s).length > max ? `${Array.from(s).slice(0, max - 1).join('')}\u2026` : s);

/** Key map label for X-ray and the key's name: HA actions read "Lamp · toggle", HA-device commands "Shield · DPAD_UP". */
export function keyActionLabel(a: { kind: string; label: string } & Record<string, unknown>, cfg?: Config, names?: ReadonlyMap<string, string>): string {
  if (a.kind === 'ha' && typeof a['service'] === 'string') {
    const ent = typeof a['entity'] === 'string' ? a['entity'] : '';
    const [first = '', more] = ent.split(' +');
    return haLabel(first, Number(more) || 0, a['service'], a.label.endsWith('(repeats)'), names);
  }
  if (a.kind === 'device' && cfg) {
    const d = (cfg.devices ?? []).find((v) => v.id === a['device']);
    if (d?.kind === 'ha') return `${d.name ?? d.id} \u00B7 ${typeof a['command'] === 'string' ? displayCommand(a['command']) : `input ${displayCommand(String(a['input']))}`}`;
  }
  // the short "tv: vol_up" form from the key map, with the command part made readable
  const cmd = a.kind === 'device' && typeof a['command'] === 'string' ? a['command'] : '';
  if (cmd && a.label.endsWith(cmd)) return a.label.slice(0, -cmd.length) + displayCommand(cmd);
  return a.label;
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
export const HA_ENTITY_ID = /^[a-z0-9_]+\.[a-z0-9_]+$/;

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
      return `${(cfg.devices ?? []).find((d) => d.id === s.device)?.name ?? s.device} · ${displayName(s.name)}`;
    case 'activity':
      return `Start ${(cfg.activities ?? []).find((a) => a.id === s.id)?.name ?? s.id}`;
    case 'alloff':
      return 'All Off';
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
      return displayName(s.name);
    case 'activity':
      return (cfg.activities ?? []).find((a) => a.id === s.id)?.name ?? pretty(s.id);
    case 'alloff':
      return 'All Off';
    case 'entity':
    case 'ha':
      return s.kind === 'entity' && s.name ? s.name : pretty(entityDomain(s.entity_id) === '' ? '' : (s.entity_id.split('.')[1] ?? ''));
    case 'widget':
      return pretty(s.type);
    default:
      return '';
  }
}

/** A typed service field: numbers, booleans, lists and objects are read as JSON, anything else stays text. Empty means "not set". */
export function parseField(v: string): unknown {
  const t = v.trim();
  if (!t) return undefined;
  if (/^(-?\d+(\.\d+)?|true|false|\[.*\]|\{.*\})$/s.test(t)) {
    try {
      return JSON.parse(t);
    } catch {
      return t;
    }
  }
  return t;
}
/** The data object of a service call from its typed fields (unset fields left out). */
export function fieldData(fields: Record<string, string>): Record<string, unknown> | undefined {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(fields)) {
    const p = parseField(v);
    if (p !== undefined) out[k] = p;
  }
  return Object.keys(out).length ? out : undefined;
}

/** The IR code an action sends, when it is one: a device command, power or input, or an inline code. */
export function resolveIr(cfg: Config, a: unknown): IrAction | null {
  if (!a || typeof a !== 'object') return null;
  const x = a as Action;
  if ('device' in x) {
    const d = (cfg.devices ?? []).find((v) => v.id === x['device']);
    if (!d || d.kind === 'ha' || d.kind === 'bt') return null;
    if ('input' in x) return d.inputs?.[String(x['input'])] ?? null;
    const c = String(x['command']);
    if (c === 'power_on') return d.power?.on ?? d.power?.toggle ?? null;
    if (c === 'power_off') return d.power?.off ?? d.power?.toggle ?? null;
    return d.commands?.[c] ?? null;
  }
  return 'proto' in x || 'pronto' in x || 'raw' in x ? (x as IrAction) : null;
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
    n += 5 + (styled || w.nav ? 1 : 0) + (w.icon && w.icon !== 'none' ? 1 : 0) + (w.nav || (w['bind'] as unknown) ? 1 : 0) + utf8(w.label ?? '') + colourBytes(w);
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
// ---------------------------------------------------------------- pixel budget (the remote cuts labels by pixels, not bytes)
export interface Meter { font: Font; px: number; used: number; over: number }
export interface Overflow extends Meter { index: number; label: string; advice: string; short?: string }
function wireWidget(page: Page, pp: PreviewPage | null, wi: number) {
  const loc = page.list ? null : wireLocate(page, pp, wi);
  const wp = pp?.wire_pages[loc?.screen ?? 0]?.page;
  return { wp, wire: loc ? wp?.widgets[loc.idx] : undefined };
}
/** Is the widget drawn icon-only? The compiled widget's value (the compiler defaults it on), else the config's. */
export function wireIconOnly(page: Page, pp: PreviewPage | null, wi: number): boolean {
  return wireWidget(page, pp, wi).wire?.icon_only ?? !!page.widgets[wi]?.icon_only;
}
/** The widget as the remote will draw it: the wire page's copy when there is a preview (icons inferred, icon-only defaulted), else a config estimate. */
function fitInputs(page: Page, pp: PreviewPage | null, wi: number): { p: { cols: number; rows: number; fixed?: boolean }; w: FitWidget } | null {
  const w = page.widgets[wi];
  if (!w) return null;
  const { wp, wire } = wireWidget(page, pp, wi);
  if (wp && wire) return { p: { cols: wp.cols, rows: wp.rows, fixed: !!(wp as { fixed?: boolean }).fixed }, w: { ...wire, label: wire.label ?? w.label ?? '' } };
  const { cols, rows } = gridOf(page);
  const nav = typeof w['nav'] === 'string' ? (w['nav'] as string) : '';
  const shape = w.shape ?? (page.list || nav ? 'row' : 'card');
  const list = !!page.list;
  return {
    p: { cols: list ? 1 : cols, rows: list ? page.list?.rows ?? 6 : rows, fixed: page.layout === 'fixed' },
    w: { kind: w.type, x: list ? 0 : w.x ?? 0, y: list ? wi : w.y ?? 0, w: list ? 1 : w.w ?? 1, h: list ? 1 : w.h ?? 1, label: w.label ?? '', shape, icon: stockIconId(w.icon ?? ''), icon_only: !!w.icon_only, trailing: !!nav && !['prev', 'home'].includes(nav), compact: !!(w as { compact?: boolean }).compact },
  };
}
/** Pixel meter of one widget's label: the font, the room, what the text needs. Null when no label is drawn. */
export function labelMeter(page: Page, pp: PreviewPage | null, wi: number, label?: string): Meter | null {
  const f = fitInputs(page, pp, wi);
  if (!f) return null;
  const w = label === undefined ? f.w : { ...f.w, label };
  const b = labelBudget(f.p, w);
  if (!b) return null;
  const used = textPx(w.label, b.font);
  return { font: b.font, px: b.px, used, over: Math.max(0, used - b.px) };
}
/** One line of advice for a label that will be cut: a hard key, an icon, the device prefix or an abbreviation. */
function adviceFor(page: Page, w: Widget, m: Meter): { advice: string; short?: string } {
  const cmd = String((w['action'] as { command?: string } | undefined)?.command ?? '');
  const label = w.label ?? '';
  if (cmd && isHardKey(cmd)) return { advice: 'this is a hard key, so remove it' };
  const prefix = (page.title ?? '').trim();
  if (prefix && label.toLowerCase().startsWith(`${prefix.toLowerCase()} `)) return { advice: `the page title already says "${prefix}": drop the prefix` };
  const short = abbreviate(label);
  if (short !== label && textPx(short, m.font) <= m.px) return { advice: `use "${short}"`, short };
  if (bestIcon(cmd, label)) return { advice: 'use an icon' };
  return { advice: 'shorten it' };
}
/** Every label on this page the remote would cut, with the pixels over and what to do about it. */
export function pageOverflow(page: Page, pp: PreviewPage | null): Overflow[] {
  const out: Overflow[] = [];
  page.widgets.forEach((w, index) => {
    const m = labelMeter(page, pp, index);
    if (m && m.over > 0) out.push({ ...m, index, label: w.label ?? '', ...adviceFor(page, w, m) });
  });
  return out;
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
export const MAX_PAGES = 252;
export const MAX_PAGE_WIDGETS = 32;
export const MAX_SOFT_KEYS = 128;
/** Why one more widget cannot go on page `pi` (page, soft-key caps), or null. Labels take no soft key. */
export function widgetCap(cfg: Config, pi: number): string | null {
  const page = cfg.pages?.[pi];
  if (page && page.widgets.length >= MAX_PAGE_WIDGETS) return `A page holds at most ${MAX_PAGE_WIDGETS} widgets.`;
  const soft = (cfg.pages ?? []).reduce((n, p) => n + p.widgets.filter((w) => w.type !== 'label').length, 0);
  if (soft >= MAX_SOFT_KEYS) return `The remote has ${MAX_SOFT_KEYS} soft keys for all pages together; remove a button or tile first.`;
  return null;
}
export function pageCap(cfg: Config): string | null {
  return (cfg.pages?.length ?? 0) >= MAX_PAGES ? `The remote holds at most ${MAX_PAGES} pages.` : null;
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
      return { config, key: t.name, message: s.kind === 'default' ? `${keyTitle(t.name)} now uses Default` : `${keyTitle(t.name)} does nothing in ${verb}` };
    }
    const action = sourceAction(s) as Action;
    return { config: setKey(cfg, layer, t.name, action), key: t.name, message: `Assigned ${describeSource(cfg, s)} to ${keyTitle(t.name)}${layer === DEFAULT_LAYER ? '' : ', overriding Default'}` };
  }
  const page = cfg.pages?.[t.page];
  if (!page) return { refused: 'There is no page to put that on.' };
  if (needsHa && !haOk && !(s.kind === 'entity' && isStateEntity(s.entity_id))) return { refused: 'This add-on version cannot store Home Assistant actions yet.' };
  if (t.kind === 'cell') {
    const cap = widgetCap(cfg, t.page);
    if (cap) return { refused: cap };
    let at = { x: t.x, y: t.y };
    const bigger = s.kind === 'widget' && s.type === 'label' ? Math.min(2, pageDims(page).cols) : Math.min(2, pageDims(page).cols);
    if (!page.list && at.y >= MAX_ROWS) return { refused: 'This page is full.' };
    const append = !page.list && at.y === pageDims(page).rows; // a drop under the last row grows the page by one row
    const room = append ? { ...page, rows: at.y + 1 } : page;
    if (!page.list && !fits(room, { ...at, w: 1, h: 1 })) {
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
      while (width > 1 && !fits(room, { x: at.x, y: at.y, w: width, h: w.h ?? 1 })) width--;
      w.w = width;
    }
    const next = clone(cfg);
    const pg = next.pages?.[t.page] as Page;
    if (append) pg.rows = at.y + 1;
    pg.widgets.push(w);
    const taps = w.type === 'tile' && typeof w.entity === 'string' ? tileDefaultTap(w.entity) : null;
    return { config: next, widget: { page: t.page, index: pg.widgets.length - 1 }, message: `Added ${w.type} ${w.label ?? ''}${taps ? ` (a tap runs ${taps})` : ''} to page ${page.title || page.id} at column ${(w.x ?? 0) + 1}, row ${(w.y ?? 0) + 1}` };
  }
  const w = page.widgets[t.index];
  if (!w) return { refused: 'That widget is gone.' };
  if (s.kind === 'widget') return { refused: 'Drop a widget type on an empty cell.' };
  if (w.type === 'tile') {
    if (s.kind === 'entity') return { config: setWidget(cfg, t.page, t.index, { entity: s.entity_id }), widget: { page: t.page, index: t.index }, message: `Tile now shows ${s.entity_id}` };
    const tap = s.kind === 'none' ? 'none' : sourceAction(s);
    if (!tap || s.kind === 'default') return { refused: 'Drop an entity to change what the tile shows, or an action to change what a tap does.' };
    if (!haOk) return { refused: 'This add-on version cannot store tile taps yet.' };
    return { config: setWidget(cfg, t.page, t.index, { tap }), widget: { page: t.page, index: t.index }, message: `Tapping tile ${w.label ?? ''} now does ${describeSource(cfg, s)}`.replace('  ', ' ') };
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

export type TemplateId = 'activities' | 'activity' | 'numbers' | 'lights' | 'devices' | 'favorites' | 'blank';
export const TEMPLATES: { id: TemplateId; name: string; note: string }[] = [
  { id: 'activities', name: 'Activities Home', note: 'One button per activity, plus All off' },
  { id: 'activity', name: 'Activity Control', note: 'Home, Apps, Extras and Numbers pages for the activity' },
  { id: 'numbers', name: 'Number Pad', note: '0-9 of the first device that has them' },
  { id: 'lights', name: 'Lights and Scenes', note: 'Tiles for Home Assistant lights' },
  { id: 'devices', name: 'Device List', note: 'A row per device, with a page of keys and inputs for each' },
  { id: 'favorites', name: 'Favorites', note: 'Compact cards for the commands and activities you use most' },
  { id: 'blank', name: 'Blank', note: 'Empty 4x6 grid' },
];

// ---------------------------------------------------------------- starter pages
/** Commands the remote's own hard keys already carry; starter pages leave them out. */
const HARD = /^(?:vol(?:ume)?_?(?:up|down|plus|minus)|ch(?:annel)?_?(?:up|down|plus|minus)|mute|(?:direction_?|arrow_?|dpad_?|cursor_?)?(?:up|down|left|right)|ok|select|center|dpad_center|play|pause|play_?pause|ff|fast_?forward|rew|rewind|stop|rec|record|red|green|yellow|blue|menu|exit|return|back|guide|info|dvr)$/;
const snake = (n: string): string => n.replace(/([a-z0-9])([A-Z])/g, '$1_$2').replace(/[\s-]+/g, '_').toLowerCase();
export const isHardKey = (cmd: string): boolean => HARD.test(snake(cmd));
const TOOLS = /^(?:tools?|options?)$/;
const UTILITY = /^(?:home|apps?|app_?list|search|settings?|setup|keyboard|sleep(?:_?timer)?)$/;
const EXTRAS = /replay|skip_?\d|(?:rew|rewind|forward|ffwd)_?\d|subtitle|caption|^cc$|audio|aspect|sleep|shuffle|repeat|angle|zoom|picture|pic_?mode/;

/** The stock icon name when this build's icon set has it; a glyph that is not shipped yet is simply not used. */
const iconIf = (name: string): string | undefined => (stockIconByName(name) ? name : undefined);

// ---------------------------------------------------------------- inputs and device icons
/** "InputHdmi1", "HDMI1", "Input HDMI 2" -> "HDMI 1" / "HDMI 2"; "InputAv" -> "AV", "InputUsb" -> "USB", "InputTv" -> "TV"; null for
 * any other command. Twin of harmony_core.key_names.normalize_input (both run tests/vectors/inputs.json). */
export function normalizeInput(name: string): string | null {
  const c = snake(name.trim().replace(/([A-Za-z])(\d)/g, '$1_$2'));
  const h = /^(?:input_)?hdmi_?(\d+)(?:_?mhl|_?arc|_?earc)?$/.exec(c);
  if (h) return `HDMI ${Number(h[1])}`;
  const p = /^input_(av|usb|tv)(?:_?(\d+))?$/.exec(c);
  return p ? (p[1] as string).toUpperCase() + (p[2] ? ` ${Number(p[2])}` : '') : null;
}
/** A device's inputs: the configured ones, else derived from commands named like InputHdmi1 (label = the normalised name). */
export function deviceInputs(d: Device): { label: string; action: Action }[] {
  if (Object.keys(d.inputs ?? {}).length) return Object.keys(d.inputs ?? {}).map((k) => ({ label: k, action: { device: d.id, input: k } }));
  const out: { label: string; action: Action }[] = [];
  for (const c of commandNames(d)) {
    const label = normalizeInput(c);
    if (label && !out.some((o) => o.label === label)) out.push({ label, action: { device: d.id, command: c } });
  }
  return out;
}

/** The editor's note for a device whose inputs come from its commands ("4 inputs found in commands"); '' when it has its own `inputs`. */
export function derivedInputsNote(d: Device): string {
  if (Object.keys(d.inputs ?? {}).length) return '';
  const n = deviceInputs(d).length;
  return n ? `${n} input${n === 1 ? '' : 's'} found in commands` : '';
}
/** Commands that select an input (shown as inputs, never as keys), whether derived or already saved as the device's `inputs`. */
const isInputCommand = (_d: Device, c: string): boolean => normalizeInput(c) !== null;
/** The inputs derived from a device's commands, with the command each one sends; none when the device has its own `inputs`. */
export function derivedInputs(d: Device): { label: string; command: string }[] {
  if (Object.keys(d.inputs ?? {}).length) return [];
  return deviceInputs(d).map((i) => ({ label: i.label, command: String((i.action as { command?: string }).command ?? '') }));
}
/**
 * Save a device's derived inputs as its own `inputs`: each row keeps its IR code under its new name, a hidden row is left out.
 * Null when nothing is left, a name is empty or two rows share a name.
 */
export function materializeInputs(cfg: Config, deviceId: string, rows: { command: string; name: string; hidden: boolean }[]): Config | null {
  const dev = (cfg.devices ?? []).find((d) => d.id === deviceId);
  if (!dev) return null;
  const keep = rows.filter((r) => !r.hidden);
  const names = keep.map((r) => r.name.trim());
  if (!keep.length || names.some((n) => !n) || new Set(names.map((n) => n.toLowerCase())).size !== names.length) return null;
  const next = clone(cfg);
  const d = (next.devices ?? []).find((x) => x.id === deviceId) as Device;
  const inputs: Record<string, unknown> = {};
  keep.forEach((r, i) => {
    const code = d.commands?.[r.command];
    if (code) inputs[trimToBytes(names[i] as string, LABEL_MAX)] = clone(code);
  });
  d.inputs = inputs as Device['inputs'];
  return next;
}

const INPUT_ICON: [RegExp, string][] = [[/macbook|laptop|notebook/i, 'laptop'], [/desktop|computer|\bpc\b|workstation/i, 'pc']];
/** Glyph for an input row: HDMI N -> hdmiN, AV, USB, TV, laptop / pc by name, else the best keyword match. */
export function inputIcon(label: string): string | undefined {
  const h = /^HDMI (\d)$/.exec(label);
  if (h) return iconIf(`hdmi${h[1]}`);
  const fixed = ({ AV: 'av', USB: 'usb', TV: 'tv' } as Record<string, string>)[label];
  if (fixed) return iconIf(fixed);
  return iconIf(INPUT_ICON.find(([re]) => re.test(label))?.[1] ?? bestIcon(label));
}

const KIND_ICON: [RegExp, string][] = [
  [/kvm/i, 'kvm'], [/soundbar|sound bar|sonos|homepod/i, 'soundbar'], [/receiver|\bavr\b|denon|marantz|onkyo|yamaha rx/i, 'receiver'],
  [/speaker|audioengine/i, 'speaker'], [/projector|beamer/i, 'projector'], [/stream|roku|apple tv|chromecast|fire ?tv/i, 'streamer'],
  [/laptop|macbook/i, 'laptop'], [/\bpc\b|computer|desktop/i, 'pc'], [/\btv\b|television|bravia|oled|qled|samsung|\blg\b/i, 'tv'],
];
const BRAND_WORDS: Record<string, string> = { shield: 'nvidia', nvidia: 'nvidia', plex: 'plex', youtube: 'youtube', kodi: 'kodi' };
/**
 * The glyph a device gets: the icon set in config, a brand logo matched by name or entity, the device kind or Home Assistant domain,
 * then keywords in the name. A Home Assistant media player skips the brand step: it is an app list, not the box itself.
 */
export function inferDeviceIcon(d: Device): string | undefined {
  if (d.icon && stockIconByName(d.icon)) return d.icon;
  const text = `${d.name ?? ''} ${d.id} ${d.entity ?? ''}`;
  const domain = d.entity ? entityDomain(d.entity) : '';
  if (d.kind === 'ha' && domain === 'media_player') return iconIf('grid_apps');
  const brand = (text.toLowerCase().match(/[a-z0-9]+/g) ?? []).map((w) => BRAND_WORDS[w]).find((n) => n && stockIconByName(n));
  if (brand) return brand;
  const kind = KIND_ICON.find(([re]) => re.test(text))?.[1];
  if (kind && stockIconByName(kind)) return kind;
  if (d.kind === 'ha' && (domain === 'switch' || domain === 'input_boolean') && !Object.keys(d.commands ?? {}).length && !Object.keys(d.inputs ?? {}).length) return iconIf('plug');
  return iconIf(bestIcon(d.name ?? d.id, d.id)) ?? iconIf('app');
}

/** `fitName`, then fewer words ("Sleep Timer" -> "Sleep") until `fits`; the full name when nothing fits. */
export function fitShort(name: string, fits: (s: string) => boolean): string {
  const full = fitName(name, fits);
  if (fits(full)) return full;
  const words = full.split(' ');
  for (let n = words.length - 1; n >= 1; n--) {
    const t = words.slice(0, n).join(' ');
    if (fits(t)) return t;
  }
  return full;
}
/** A page title that fits the header beside the back chevron (b18, 102 px): `full`, else `short` when given, else fewer words. */
export function fitTitle(full: string, short?: string, px = 100): string {
  const ok = (t: string): boolean => textPx(t, 'b18') <= px;
  if (ok(full)) return full;
  if (short && ok(short)) return short;
  const words = full.split(' ');
  for (let n = words.length - 1; n >= 1; n--) if (ok(words.slice(0, n).join(' '))) return words.slice(0, n).join(' ');
  return full;
}
/** A device that is a list of apps (a Home Assistant media player): its page uses the Apps layout. */
const isAppDevice = (d: Device): boolean => inferDeviceIcon(d) === 'grid_apps';

/**
 * The physical boxes of a config, in first-member order: devices sharing a `group` are one box, the rest are a box each.
 * Name and icon: the `groups:` entry, else the first member's own name and icon (its brand or kind when it sets none; a Home
 * Assistant app list gives way to the next member's glyph, since it is not the box itself).
 */
export function deviceGroups(cfg: Config): { id: string; name: string | undefined; icon: string | undefined; members: Device[] }[] {
  const out: { id: string; name: string | undefined; icon: string | undefined; members: Device[] }[] = [];
  const byKey = new Map<string, Device[]>();
  for (const d of cfg.devices ?? []) {
    const k = d.group ? `g:${d.group}` : `d:${d.id}`;
    const m = byKey.get(k);
    if (m) m.push(d);
    else {
      const members = [d];
      byKey.set(k, members);
      out.push({ id: d.group ?? d.id, name: undefined, icon: undefined, members });
    }
  }
  for (const g of out) {
    const first = g.members[0] as Device;
    const own = groupGlyph(first, g.members);
    const o = first.group ? cfg.groups?.[g.id] : undefined;
    g.name = o?.name ?? first.name;
    g.icon = o?.icon && stockIconByName(o.icon) ? o.icon : own;
  }
  return out;
}
const groupGlyph = (first: Device, members: Device[]): string | undefined => {
  const icon = inferDeviceIcon(first);
  if (icon !== 'grid_apps' || first.icon) return icon;
  return members.map((m) => inferDeviceIcon(m)).find((i) => i && i !== 'grid_apps') ?? icon;
};

/** The glyph for an activity card: a brand logo matched by name (Shield TV -> NVIDIA), else the best keyword match, else the TV. */
export function activityIcon(name: string): string | undefined {
  const brand = (name.toLowerCase().match(/[a-z0-9]+/g) ?? []).map((w) => BRAND_WORDS[w]).find((n) => n && stockIconByName(n));
  return brand ?? iconIf(bestIcon(name)) ?? iconIf('tv');
}

/** The pages a template adds, in order; the first is the one that opens. Ids are unique among the config's pages and each other. */
export function templatePages(cfg: Config, id: TemplateId, ctx: { entities?: { entity_id: string; name?: string }[]; activity?: string; device?: string } = {}): Page[] {
  const devices = cfg.devices ?? [];
  const taken = new Set((cfg.pages ?? []).map((p) => p.id));
  const pid = (base: string): string => {
    let n = 1;
    let out = base;
    while (taken.has(out)) out = `${base}_${++n}`;
    taken.add(out);
    return out;
  };
  const used = new Set((cfg.pages ?? []).flatMap((pg) => pg.widgets.map((w) => w.id)));
  const mk = (base: string) => {
    let n = 1;
    while (used.has(`${base}${n}`)) n++;
    used.add(`${base}${n}`);
    return `${base}${n}`;
  };
  const btn = (x: number, y: number, label: string, action: Action, extra: Partial<Widget> = {}, w = 2): Widget => ({ type: 'button', id: mk('btn'), x, y, w, h: 1, label: trimToBytes(label, LABEL_MAX), action, ...extra });
  /** A command button: the display name (abbreviated only when over budget); a FIXED page passes its span in `b.w` (12/6/4/3). */
  const cmdBtn = (dev: Device, cmd: string, grid: { cols: number; rows: number; fixed?: boolean }, b: Box, extra: Partial<Widget> = {}): Widget => {
    const icon = bestIcon(cmd);
    const only = !!icon && (extra.icon_only ?? (!grid.fixed && grid.cols === 4 && b.w === 1));
    const fits = (s: string): boolean => {
      const lb = labelBudget(grid, { kind: 'button', ...b, label: s, shape: extra.shape ?? 'card', icon: icon ? 1 : 0, icon_only: only, compact: !!extra.compact });
      return !lb || textPx(s, lb.font) <= lb.px;
    };
    const name = fitShort(cmd, fits);
    const cut = !fits(name) && !!icon; // a word that cannot fit: the glyph alone
    return btn(b.x, b.y, name, { device: dev.id, command: cmd }, { ...(icon ? { icon } : {}), ...(only || cut ? { icon_only: true } : {}), ...extra, ...(cut ? { icon_only: true } : {}) }, b.w);
  };
  /** A full-width row of a FIXED page (44 px): glyph, label, and a chevron when it opens a page. */
  const rowBtn = (label: string, icon: string | undefined, y: number, more: Partial<Widget>): Widget => ({ type: 'button', id: mk('btn'), x: 0, y, w: 12, h: 1, shape: 'row', label: trimToBytes(label, LABEL_MAX), ...(icon ? { icon } : {}), ...more });
  const fixedPage = (page: Omit<Page, 'cols' | 'rows' | 'layout'>): Page => ({ layout: 'fixed', cols: 12, rows: Math.max(1, ...page.widgets.map((w) => (w.y ?? 0) + 1)), ...page });
  const act = (cfg.activities ?? []).find((a) => a.id === ctx.activity);
  const actDevices = (act?.devices ?? []).map((d) => devices.find((x) => x.id === d.device)).filter((d): d is Device => !!d);
  const pool = actDevices.length ? actDevices : [devices.find((x) => x.id === ctx.device) ?? devices[0]].filter((d): d is Device => !!d);

  /** 3x4 keypad (FIXED, 69 x 52 pills): digits as b20, Prev Ch and Enter as glyphs. */
  const numbersPage = (page: string, d: Device | undefined, back: boolean): Page => {
    const widgets: Widget[] = [];
    const names = d ? commandNames(d) : [];
    for (let n = 1; n <= 10 && d; n++) {
      const c = n % 10;
      const cmd = names.find((k) => RE.numbers.test(k.toLowerCase()) && k.endsWith(String(c)));
      if (cmd) widgets.push(btn(c === 0 ? 4 : ((n - 1) % 3) * 4, c === 0 ? 3 : Math.floor((n - 1) / 3), String(c), { device: d.id, command: cmd }, { shape: 'pill' }, 4));
    }
    const side = (re: RegExp, x: number, label: string, icon: string | undefined) => {
      const cmd = names.find((k) => re.test(snake(k)));
      if (cmd && d) widgets.push(btn(x, 3, label, { device: d.id, command: cmd }, { shape: 'pill', ...(icon ? { icon, icon_only: true } : {}) }, 4));
    };
    side(/^(?:prev(?:ious)?_?ch(?:annel)?|last(?:_?ch(?:annel)?)?)$/, 0, 'Prev Ch', iconIf('prev_ch'));
    side(/^enter$/, 8, 'Enter', iconIf('enter'));
    return fixedPage({ id: page, title: 'Numbers', ...(back ? { back: true } : {}), widgets });
  };

  switch (id) {
    case 'activities': {
      // Home: activities as 108 x 80 cards (two a row), then 44 px rows to Devices, Lights and Favorites (when those pages exist) and All Off
      const acts = (cfg.activities ?? []).slice(0, 8);
      const pages = cfg.pages ?? [];
      const compact = acts.length > 2; // 56 px cards keep Devices, Lights and All Off on the first screen
      const widgets: Widget[] = acts.map((a, i) => btn((i % 2) * 6, Math.floor(i / 2), dn(a.name, a.id), { activity: a.id }, { align: 'left', icon_only: false, ...(compact ? { compact: true } : {}), icon: activityIcon(a.name ?? a.id) }, 6));
      let y = Math.ceil(acts.length / 2);
      const nav = (label: string, icon: string, target: string | undefined) => {
        if (target) widgets.push(rowBtn(label, iconIf(icon), y++, { nav: target, align: 'left' }));
      };
      const devT = (pages.find((p) => p.id === 'devices') ?? pages.find((p) => p.kind === 'favorites' && p.id !== 'favorites'))?.id;
      // All Off always stays on the first screen: it comes before the rows that may page
      nav('Devices', 'list', devT);
      widgets.push(rowBtn('All Off', iconIf('power'), y++, { action: { activity: 'off' }, role: 'danger', align: 'left' }));
      nav('Lights', 'bulb', pages.find((p) => p.id === 'lights')?.id);
      nav('Favorites', 'star', pages.find((p) => p.id === 'favorites')?.id);
      const hasHome = pages.some((p) => p.kind === 'home');
      return [fixedPage({ id: pid('home'), ...(hasHome ? {} : { kind: 'home' as const }), title: 'Activities', widgets })];
    }
    case 'activity': {
      // Two app cards, glyph keys (span 3) and rows to Apps, Extras, Numbers and Inputs; the sub-pages are FIXED too.
      type Cmd = { dev: Device; cmd: string };
      const all: Cmd[] = [];
      for (const dev of pool) for (const cmd of commandNames(dev)) if (!isHardKey(cmd) && !all.some((c) => c.cmd === cmd)) all.push({ dev, cmd });
      const numbersDev = pool.find((dv) => commandNames(dv).some((n) => groupCommand(n) === 'numbers'));
      const rest = all.filter((c) => groupCommand(c.cmd) !== 'numbers' && !/^(?:enter|prev_?ch(?:annel)?|last)$/.test(snake(c.cmd)));
      // a Bluetooth device named like the activity (Shield TV -> Shield) lends its Home, Search, Settings and Keyboard keys
      const words = (act?.name ?? '').toLowerCase().match(/[a-z0-9]{4,}/g) ?? [];
      const lenders = devices.filter((dv) => !pool.includes(dv) && dv.kind === 'bt' && words.some((w) => `${dv.name ?? ''} ${dv.id}`.toLowerCase().includes(w)));
      const lent: Cmd[] = lenders.flatMap((dv) => commandNames(dv).filter((cmd) => UTILITY.test(snake(cmd)) && !isHardKey(cmd)).map((cmd) => ({ dev: dv, cmd })));
      const utils = [...rest.filter((c) => UTILITY.test(snake(c.cmd))), ...lent.filter((c) => !rest.some((r) => r.cmd === c.cmd))].slice(0, 4);
      const extras = rest.filter((c) => !utils.includes(c) && EXTRAS.test(snake(c.cmd))).slice(0, 16);
      // device menu commands (Tools) belong on the device page, not among the apps
      const apps = rest.filter((c) => !utils.includes(c) && !extras.includes(c) && groupCommand(c.cmd) === 'other' && !TOOLS.test(snake(c.cmd)));
      const inDev = pool.reduce<Device | undefined>((best, d) => (deviceInputs(d).length > (best ? deviceInputs(best).length : 1) ? d : best), undefined);
      const name = fitTitle(pool.length === 1 ? (pool[0]?.name ?? pool[0]?.id ?? 'Control') : (act?.name ?? pool[0]?.name ?? 'Control'));
      const ids = { home: pid('activity'), apps: pid('apps'), extras: pid('extras'), numbers: pid('numbers'), inputs: pid('inputs') };
      const nav: { page: string; label: string; icon: string | undefined }[] = [];
      if (apps.length > 2) nav.push({ page: ids.apps, label: 'Apps', icon: iconIf('grid_apps') ?? iconIf('app') });
      if (inDev) nav.push({ page: ids.inputs, label: 'Inputs', icon: iconIf('input') });
      if (extras.length) nav.push({ page: ids.extras, label: 'Extras', icon: iconIf('list') });
      if (numbersDev) nav.push({ page: ids.numbers, label: 'Numbers', icon: iconIf('numpad') });
      const g = { cols: 12, rows: 16, fixed: true };
      const home: Widget[] = [];
      let y = 0;
      apps.slice(0, 2).forEach((c, i) => home.push(cmdBtn(c.dev, c.cmd, g, { x: i * 6, y, w: 6, h: 1 }, { shape: 'card', align: 'left', icon_only: false })));
      if (apps.length) y++;
      utils.forEach((c, i) => home.push(cmdBtn(c.dev, c.cmd, g, { x: i * 3, y, w: 3, h: 1 }, { icon_only: !!bestIcon(c.cmd) })));
      if (utils.length) y++;
      nav.forEach((n) => home.push(rowBtn(n.label, n.icon, y++, { nav: n.page, align: 'left' })));
      const pages: Page[] = [fixedPage({ id: ids.home, title: name, back: true, ...(ctx.activity ? { activity: ctx.activity } : {}), widgets: home })];
      if (apps.length > 2) {
        // up to 4 apps are 80 px cards; 5 to 8 are 56 px compact cards (the compiler adds page 2 beyond that)
        const compact = apps.length > 4;
        const shown = apps.slice(0, 16);
        pages.push(fixedPage({ id: ids.apps, title: 'Apps', back: true, widgets: shown.map((c, i) => cmdBtn(c.dev, c.cmd, g, { x: (i % 2) * 6, y: Math.floor(i / 2), w: 6, h: 1 }, { shape: 'card', align: 'left', icon_only: false, ...(compact ? { compact: true } : {}) })) }));
      }
      if (inDev) {
        const ins = deviceInputs(inDev).slice(0, 16);
        pages.push(fixedPage({ id: ids.inputs, title: fitTitle(`${dn(inDev.name, inDev.id)} Inputs`, 'Inputs'), back: true, widgets: ins.map((i, n) => rowBtn(i.label, inputIcon(i.label), n, { action: i.action, align: 'left' })) }));
      }
      if (extras.length) pages.push(fixedPage({ id: ids.extras, title: 'Extras', back: true, widgets: extras.map((c, i) => cmdBtn(c.dev, c.cmd, g, { x: (i % 3) * 4, y: Math.floor(i / 3), w: 4, h: 1 }, { icon_only: false })) }));
      if (numbersDev) pages.push({ ...numbersPage(ids.numbers, numbersDev, true) });
      return pages;
    }
    case 'numbers': {
      const d = devices.find((dv) => commandNames(dv).some((n) => groupCommand(n) === 'numbers')) ?? devices[0];
      return [numbersPage(pid('numbers'), d, true)];
    }
    case 'lights': {
      // 108 x 64 tiles, two a row: glyph and state on top, name below
      const lights = (ctx.entities ?? []).filter((e) => entityDomain(e.entity_id) === 'light').slice(0, 8);
      const widgets: Widget[] = lights.map((e, i) => ({ type: 'tile', id: mk('tile'), x: (i % 2) * 6, y: Math.floor(i / 2), w: 6, h: 1, label: trimToBytes(e.name ?? e.entity_id, LABEL_MAX), entity: e.entity_id, icon: 'bulb' }));
      return [fixedPage({ id: pid('lights'), title: 'Lights', back: true, widgets })];
    }
    case 'devices': {
      // Every device with something to press gets a row (a glyph on each) and a page: power keys, commands as keys, inputs inline
      // (up to 4) or behind an Inputs row (5 or more), and All Commands for the keys that do not fit.
      const listId = pid('devices');
      type Plan = { id: string; name: string | undefined; icon: string | undefined; power: Widget[]; keys: Widget[]; inputs: { label: string; action: Action }[]; apps: Widget[] };
      const plans: Plan[] = [];
      const key = (dev: Device, cmd: string): Widget => {
        const icon = bestIcon(cmd);
        const label = fitShort(cmd, (s) => textPx(s, 'r12') <= 61);
        return { type: 'button', id: mk('btn'), x: 0, y: 0, w: 4, h: 1, label: trimToBytes(label, LABEL_MAX), action: { device: dev.id, command: cmd }, ...(icon ? { icon } : {}) };
      };
      for (const g of deviceGroups({ ...cfg, devices: devices.slice(0, 16) })) {
        const power: Widget[] = [];
        const keys: Widget[] = [];
        const apps: Widget[] = [];
        const inputs: { label: string; action: Action }[] = [];
        for (const d of g.members) {
          const own = deviceInputs(d);
          if (d.power?.on || d.power?.off || d.power?.toggle) {
            const { on, off, toggle } = d.power;
            const mk1 = (label: string, command: string, extra: Partial<Widget> = {}): Widget => ({ type: 'button', id: mk('btn'), x: 0, y: 0, w: 4, h: 1, label, icon: 'power', action: { device: d.id, command }, ...extra });
            if (on) power.push(mk1('Power On', 'power_on', { role: 'primary' }));
            if (off) power.push(mk1('Power Off', 'power_off'));
            if (toggle && !on && !off) power.push(mk1('Power', 'power_on'));
          }
          const ks = commandNames(d).filter((c) => !isHardKey(c) && !isInputCommand(d, c) && !/^power_(?:on|off)$/.test(c)).map((c) => key(d, c));
          if (isAppDevice(d) && ks.length && !own.length) apps.push(...ks);
          else keys.push(...ks);
          for (const i of own) if (!inputs.some((o) => o.label === i.label)) inputs.push(i);
        }
        if (power.length || keys.length || inputs.length || apps.length) plans.push({ id: pid(`dev_${g.id}`.slice(0, 31)), name: g.name, icon: g.icon, power, keys, inputs, apps });
      }
      const hasFav = (cfg.pages ?? []).some((p) => p.kind === 'favorites');
      const rowH = (n: number): number => (n ? n * 44 + (n - 1) * 2 : 0);
      const keysH = (k: number): number => (k ? k * 52 + (k - 1) * 8 : 0);
      const extra: Page[] = [];
      const grid = (ws: Widget[], y0 = 0): Widget[] => ws.map((w, i) => ({ ...w, x: (i % 3) * 4, y: y0 + Math.floor(i / 3) }));
      const devPages: Page[] = [];
      for (const p of plans) {
        const title = fitTitle(trimToBytes(dn(p.name, p.id), LABEL_MAX));
        const inline = p.inputs.length <= 4;
        const allKeys = [...p.power, ...p.keys];
        // Apps layout: cards for up to 4 apps, compact cards from 5 (the page pages when it runs long)
        const compact = p.apps.length > 4;
        const nApps = p.apps.length;
        const appsH = nApps ? Math.ceil(nApps / 2) * (compact ? 56 : 80) + 8 : 0;
        const cap = Math.max(276 - appsH, keysH(2) + 8 + rowH(1)); // two key rows and the All Commands row stay, the page pages for the rest
        let nav = inline ? p.inputs.length : p.inputs.length ? 1 : 0;
        // keys that do not fit above the rows go to All Commands, which then takes one row itself
        const room = (n: number): number => {
          let k = 0;
          while (k < 16 && keysH(k + 1) + (n ? 8 + rowH(n) : 0) <= cap && k * 3 < allKeys.length) k++;
          return k * 3;
        };
        const onePage = 12; // a full page of plain command keys: up to this many all stay on the page, which then pages
        let fit = allKeys.length <= onePage ? allKeys.length : Math.min(allKeys.length, room(nav));
        if (fit < allKeys.length) {
          nav += 1;
          fit = Math.min(allKeys.length, room(nav));
        }
        const shown = allKeys.slice(0, fit);
        const rest = allKeys.slice(fit);
        const widgets: Widget[] = grid(shown);
        let y = Math.ceil(shown.length / 3);
        const inputsId = p.inputs.length > 4 ? pid(`${p.id}_in`.slice(0, 31)) : '';
        const restId = rest.length ? pid(`${p.id}_all`.slice(0, 31)) : '';
        if (inline) for (const i of p.inputs) widgets.push(rowBtn(i.label, inputIcon(i.label), y++, { action: i.action }));
        else if (p.inputs.length) widgets.push(rowBtn('Inputs', iconIf('input'), y++, { nav: inputsId }));
        const fitsCard = (s: string): boolean => {
          const lb = labelBudget({ cols: 12, rows: 16, fixed: true }, { kind: 'button', x: 0, y: 0, w: 6, h: 1, label: s, shape: 'card', icon: 1, icon_only: false, compact });
          return !lb || textPx(s, lb.font) <= lb.px;
        };
        p.apps.forEach((k, i) => {
          const cmd = String((k['action'] as { command?: string }).command ?? k.label ?? '');
          const label = fitShort(cmd, fitsCard);
          const cut = !fitsCard(label) && !!k.icon;
          widgets.push({ ...k, label: trimToBytes(label, LABEL_MAX), w: 6, x: (i % 2) * 6, y: y + Math.floor(i / 2), shape: 'card', align: 'left', icon_only: cut, ...(compact ? { compact: true } : {}) });
        });
        y += Math.ceil(nApps / 2);
        if (rest.length) widgets.push(rowBtn('All Commands', iconIf('list'), y++, { nav: restId }));
        devPages.push({ id: p.id, layout: 'fixed', cols: 12, rows: Math.max(1, y), title, widgets });
        if (!inline) {
          const ws = p.inputs.map((i, n) => rowBtn(i.label, inputIcon(i.label), n, { action: i.action }));
          extra.push({ id: inputsId, layout: 'fixed', cols: 12, rows: ws.length, title: fitTitle(`${title} Inputs`, 'Inputs'), widgets: ws });
        }
        if (rest.length) {
          const ws = grid(rest);
          extra.push({ id: restId, layout: 'fixed', cols: 12, rows: Math.ceil(ws.length / 3), title: 'Commands', widgets: ws });
        }
      }
      const rows = plans.map((p, i): Widget => rowBtn(dn(p.name, p.id), p.icon, i, { nav: p.id }));
      const list: Page = { id: listId, layout: 'fixed', cols: 12, rows: Math.max(1, rows.length), ...(hasFav ? {} : { kind: 'favorites' as const }), title: 'Devices', widgets: rows };
      return [list, ...devPages, ...extra];
    }
    case 'favorites': {
      // Starred commands and activities from any device: compact cards, two to a row.
      const picks: { label: string; icon: string | undefined; action: Action }[] = [];
      for (const a of (cfg.activities ?? []).slice(0, 4)) picks.push({ label: dn(a.name, a.id), icon: activityIcon(a.name ?? a.id), action: { activity: a.id } });
      for (const d of devices) for (const c of commandNames(d)) if (!isHardKey(c) && !isInputCommand(d, c) && !/^(?:power|num|digit)/i.test(snake(c)) && !TOOLS.test(snake(c)) && picks.length < 12) picks.push({ label: fitShort(c, (s) => textPx(s, 'b13') <= 100 && !s.split(' ').some((w) => textPx(w, 'b13') > 54)), icon: bestIcon(c) || undefined, action: { device: d.id, command: c } });
      const widgets: Widget[] = picks.slice(0, 12).map((p, i) => ({ type: 'button', id: mk('btn'), x: (i % 2) * 6, y: Math.floor(i / 2), w: 6, h: 1, compact: true, icon_only: false, label: trimToBytes(p.label, LABEL_MAX), ...(p.icon ? { icon: p.icon } : {}), action: p.action }));
      return [{ id: pid('favorites'), layout: 'fixed', cols: 12, rows: Math.max(1, Math.ceil(widgets.length / 2)), title: 'Favorites', widgets }];
    }
    default:
      return [{ id: pid('blank'), cols: 4, rows: 6, title: 'New Page', widgets: [] }];
  }
}
/** The first page a template adds (the others, when it adds several, come from `templatePages`). */
export const templatePage = (cfg: Config, id: TemplateId, ctx: Parameters<typeof templatePages>[2] = {}): Page => templatePages(cfg, id, ctx)[0] as Page;
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
    if (v === undefined || v === null || v === '' || (v === false && k !== 'back')) delete p[k];
    else p[k] = v;
  }
  return next;
}

// ---------------------------------------------------------------- Studio checks (shown above the canvas)
export interface StudioCheck {
  kind: 'label' | 'glyph' | 'action';
  msg: string;
  /** label checks: the abbreviation that fits, when that is the advice */
  short?: string;
  /** the page to show and the widget to select; absent for a device */
  page?: number;
  index?: number;
}
/** A device whose glyph cannot be found: not set in config and nothing in the name, kind or entity matches. */
export const deviceHasGlyph = (d: Device): boolean => !!inferDeviceIcon(d);
/**
 * What the Studio warns about before a config is applied: a label over its class budget (the remote would cut it), a device
 * with no glyph, and a row with no action (it would do nothing when tapped). `preview(i)` is page i's compiled preview, or null.
 */
export function studioChecks(cfg: Config, preview: (page: number) => PreviewPage | null = () => null, needPreview = false): StudioCheck[] {
  const out: StudioCheck[] = [];
  (cfg.pages ?? []).forEach((pg, pi) => {
    const pp = preview(pi);
    // with needPreview a label is judged only on the compiled page: the config estimate flags captions the compiler will shorten
    for (const o of needPreview && !pp ? [] : pageOverflow(pg, pp)) out.push({ kind: 'label', page: pi, index: o.index, msg: `${pg.title || pg.id}: "${o.label}" is ${o.over} px too wide for its ${o.font} slot; ${o.advice}`, ...(o.short ? { short: o.short } : {}) });
    pg.widgets.forEach((w, wi) => {
      if (w.type === 'button' && w.shape === 'row' && !w['action'] && !w.nav) out.push({ kind: 'action', page: pi, index: wi, msg: `${pg.title || pg.id}: the row "${w.label ?? ''}" does nothing; give it an action or a page` });
    });
  });
  for (const d of cfg.devices ?? []) if (!deviceHasGlyph(d)) out.push({ kind: 'glyph', msg: `Device "${d.name ?? d.id}" has no glyph; pick an icon for it` });
  return out;
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

/** Which compiled screen a just-dropped widget landed on. 'stale': the config is no longer the one the drop made (undo, a later edit); 'wait': the preview predates the drop. */
export function placedScreen(current: unknown, made: unknown, pl: { page: number; index: number }, pp: PreviewPage | null): 'stale' | 'wait' | { screen: number; total: number } {
  if (current !== made) return 'stale';
  const pg = (current as Config).pages?.[pl.page];
  const loc = pg ? wireLocate(pg, pp, pl.index) : null;
  return loc && pp ? { screen: loc.screen, total: pp.wire_pages.length } : 'wait';
}
/** Most rows a page grid can have (the compiler's cap). */
export const MAX_ROWS = 16;

// ---------------------------------------------------------------- config widgets <-> wire widgets
/** For each compiled screen, the config widget index behind each compiled widget (the compiler reorders and splits; `src` says where each came from). */
export function wireSources(p: Page, pp: PreviewPage | null): number[][] {
  const promoted = promotedIndex(p);
  const real = p.widgets.map((_, i) => i).filter((i) => p.list || i !== promoted);
  let off = 0;
  return (pp?.wire_pages ?? []).map((s) => {
    const out = s.page.widgets.map((w, k) => (typeof w.src === 'number' && w.src >= 0 ? w.src : (real[off + k] ?? -1)));
    off += s.page.widgets.length;
    return out;
  });
}
/** For each config widget, its index on compiled screen `screen` (or -1: promoted header label, drawn on another screen). */
export function wireMap(p: Page, pp: PreviewPage | null, screen = 0): number[] {
  const map = p.widgets.map(() => -1);
  (wireSources(p, pp)[screen] ?? []).forEach((ci, k) => {
    if (ci >= 0 && ci < map.length) map[ci] = k;
  });
  return map;
}
/** Which compiled screen draws config widget `wi`, and its index there; null when none does. */
export function wireLocate(p: Page, pp: PreviewPage | null, wi: number): { screen: number; idx: number } | null {
  const all = wireSources(p, pp);
  for (let screen = 0; screen < all.length; screen++) {
    const idx = (all[screen] ?? []).indexOf(wi);
    if (idx >= 0) return { screen, idx };
  }
  return null;
}
/** Grid of the drawn page: v2 shifts rows up by one when a header label was promoted to the title. */
export function gridOf(p: Page): { cols: number; rows: number; shift: number } {
  const { cols, rows } = pageDims(p);
  const shift = promotedIndex(p) >= 0 && rows > 1 ? 1 : 0;
  return { cols, rows: rows - shift, shift };
}
