// Pure edits and models for activities: devices and inputs, the start/stop timeline, "who handles it".
import type { Activity, Config, Device } from './model';
import { displayCommand } from './key-names';
import { commandNames } from './bt';
import { trimBytes } from './codes';
import { activityOf } from './refs';
import { addPage, describeAction, firstFree, pageDims, templatePages, trimToBytes, uniqueWidgetId, LABEL_MAX, type Action } from './studio-model';

const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
type Rec = Record<string, unknown>;
export const isRefused = (r: Config | { refused: string }): r is { refused: string } => typeof (r as { refused?: unknown }).refused === 'string';
export const MAX_DEVICES = 8;
export const MAX_STEPS = 16;

function edit(cfg: Config, id: string, fn: (a: Activity, next: Config) => void): Config {
  const next = clone(cfg);
  const a = (next.activities ?? []).find((x) => x.id === id);
  if (a) fn(a, next);
  return next;
}
export function activityId(cfg: Config, name: string): string {
  const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 24) || 'activity';
  const base = slug === 'off' ? 'off_activity' : slug;
  const used = new Set((cfg.activities ?? []).map((a) => a.id));
  let id = base;
  for (let i = 2; used.has(id); i++) id = `${base}_${i}`;
  return id;
}
export const MAX_ACTIVITIES = 16;
export function addActivity(cfg: Config, name: string, devices: Activity['devices'] = []): { config: Config; id: string } {
  const id = activityId(cfg, name);
  const next = clone(cfg);
  next.activities = [...(next.activities ?? []), { id, name: trimBytes(name.trim()) || id, devices }];
  return { config: next, id };
}
export const renameActivity = (cfg: Config, id: string, name: string): Config => edit(cfg, id, (a) => void (a.name = name));

/** Remove an activity and everything that started it: key/widget actions, the page that auto-shows. */
export function deleteActivity(cfg: Config, id: string): Config {
  const next = clone(cfg);
  next.activities = (next.activities ?? []).filter((a) => a.id !== id);
  const isIt = (a: unknown) => !!a && typeof a === 'object' && (a as Rec)['activity'] === id;
  for (const [k, a] of Object.entries(next.keys ?? {})) if (isIt(a)) delete (next.keys as Rec)[k];
  for (const act of next.activities ?? []) for (const [k, a] of Object.entries(act.keys ?? {})) if (isIt(a)) delete (act.keys as Rec)[k];
  for (const p of next.pages ?? []) {
    if ((p as { activity?: string }).activity === id) delete (p as { activity?: string }).activity;
    for (const w of p.widgets) if (isIt(w['action'])) w['action'] = { none: true };
  }
  const meta = next.editor?.['activities'] as Rec | undefined;
  if (meta) delete meta[id];
  return next;
}

export function addActivityDevice(cfg: Config, id: string, device: string, input?: string): Config | { refused: string } {
  const a = activityOf(cfg, id);
  if (!a) return { refused: 'No such activity' };
  if ((a.devices ?? []).some((d) => d.device === device)) return { refused: 'That device is already in this activity' };
  if ((a.devices ?? []).length >= MAX_DEVICES) return { refused: `An activity holds at most ${MAX_DEVICES} devices` };
  return edit(cfg, id, (x) => void (x.devices = [...(x.devices ?? []), { device, ...(input ? { input } : {}) }]));
}
export const removeActivityDevice = (cfg: Config, id: string, device: string): Config =>
  edit(cfg, id, (a, next) => {
    a.devices = (a.devices ?? []).filter((d) => d.device !== device);
    const m = metaOf(next, id);
    if (m.chain) m.chain = m.chain.filter((x) => x !== device);
  });
export const setActivityInput = (cfg: Config, id: string, device: string, input: string | null): Config =>
  edit(cfg, id, (a) => {
    const d = (a.devices ?? []).find((x) => x.device === device);
    if (!d) return;
    if (input) d.input = input;
    else delete d.input;
  });

// ---------------------------------------------------------------- editor metadata
interface ActMeta {
  chain?: string[];
  handlers?: Record<string, string>;
  /** hard keys written by "who handles it": key -> the action it wrote (a key edited since is left alone) */
  filled?: Record<string, Rec>;
}
function metaOf(cfg: Config, id: string): ActMeta {
  cfg.editor = cfg.editor ?? {};
  const acts = (cfg.editor['activities'] = (cfg.editor['activities'] as Record<string, ActMeta> | undefined) ?? {}) as Record<string, ActMeta>;
  return (acts[id] = acts[id] ?? {});
}
/** Editor metadata stays far below the server's caps (65536 bytes, 5000 nodes): the editor writes only ids it can show. */
export const EDITOR_SOFT_MAX = 32 * 1024;
export const editorBytes = (cfg: Config): number => (cfg.editor ? JSON.stringify(cfg.editor).length : 0);
/** The config with `editor.activities` reduced to what the editor can use: entries of existing activities only, chains of
 *  that activity's devices, short strings, at most 128 filled keys; `filled` (a convenience) goes first if still over the soft cap.
 *  Returns the same object when nothing needs to change. */
export function boundEditor(cfg: Config): Config {
  const acts = cfg.editor?.['activities'] as Record<string, ActMeta> | undefined;
  if (!acts) return cfg;
  const keep: Record<string, ActMeta> = {};
  for (const a of cfg.activities ?? []) {
    const m = acts[a.id];
    if (!m || typeof m !== 'object') continue;
    const devs = new Set((a.devices ?? []).map((d) => d.device));
    const out: ActMeta = {};
    if (Array.isArray(m.chain)) out.chain = m.chain.filter((d) => devs.has(d)).slice(0, 16);
    if (m.handlers) out.handlers = Object.fromEntries(Object.entries(m.handlers).filter(([k, v]) => typeof v === 'string' && k.length <= 100).map(([k, v]) => [k, v.slice(0, 100)]).slice(0, 32));
    if (m.filled) out.filled = Object.fromEntries(Object.entries(m.filled).slice(0, 128));
    keep[a.id] = out;
  }
  const next = { ...cfg, editor: { ...cfg.editor, activities: keep } } as Config;
  if (editorBytes(next) > EDITOR_SOFT_MAX) for (const m of Object.values(keep)) delete m.filled;
  return JSON.stringify(next.editor) === JSON.stringify(cfg.editor) ? cfg : next;
}
export const readMeta = (cfg: Config, id: string): ActMeta => ((cfg.editor?.['activities'] as Record<string, ActMeta> | undefined)?.[id] ?? {}) as ActMeta;

/** Visual order of the device cards: the saved chain, then any device not in it. */
export function chainOrder(cfg: Config, id: string): string[] {
  const devs = (activityOf(cfg, id)?.devices ?? []).map((d) => d.device);
  const saved = (readMeta(cfg, id).chain ?? []).filter((d) => devs.includes(d));
  return [...saved, ...devs.filter((d) => !saved.includes(d))];
}
export function moveChain(cfg: Config, id: string, from: number, to: number): Config {
  const order = chainOrder(cfg, id);
  if (from < 0 || to < 0 || from >= order.length || to >= order.length || from === to) return cfg;
  const [d] = order.splice(from, 1);
  order.splice(to, 0, d as string);
  const next = clone(cfg);
  metaOf(next, id).chain = order;
  return next;
}

// ---------------------------------------------------------------- timeline
export type TItem =
  | { kind: 'auto'; label: string; device: string; ha: boolean }
  | { kind: 'gap'; ms: number }
  | { kind: 'user'; index: number; label: string; action: Rec };

const hasPower = (d: Device) => !!(d.power?.on || d.power?.toggle || d.power?.off);
/** The steps of a start or stop sequence in the order they run, with generated ones marked `auto`. */
export function timeline(cfg: Config, id: string, when: 'start' | 'stop'): { items: TItem[]; totalMs: number } {
  const act = activityOf(cfg, id);
  const order = chainOrder(cfg, id);
  const devs = new Map((cfg.devices ?? []).map((d) => [d.id, d]));
  const items: TItem[] = [];
  const entry = (dev: string) => (act?.devices ?? []).find((d) => d.device === dev);
  const userItems = (list: 'steps' | 'off'): TItem[] =>
    (act?.[list] ?? []).map((a, index) => ({ kind: 'user' as const, index, label: describeAction(cfg, a), action: a }));
  if (when === 'start') {
    for (const dv of order) {
      const d = devs.get(dv);
      if (!d) continue;
      const name = d.name ?? d.id;
      if (hasPower(d)) {
        items.push({ kind: 'auto', label: d.power?.toggle && !d.power.on ? `Power ${name} (toggle)` : `Power On ${name}`, device: dv, ha: d.kind === 'ha' });
        if (d.settle_ms) items.push({ kind: 'gap', ms: d.settle_ms });
      }
      const input = entry(dv)?.input;
      if (input) {
        items.push({ kind: 'auto', label: `${name} → ${displayCommand(input)}`, device: dv, ha: d.kind === 'ha' });
        if (d.settle_ms) items.push({ kind: 'gap', ms: d.settle_ms });
      }
    }
    items.push(...userItems('steps'));
  } else {
    items.push(...userItems('off'));
    for (const dv of [...order].reverse()) {
      const d = devs.get(dv);
      if (d && hasPower(d)) items.push({ kind: 'auto', label: d.power?.toggle && !d.power.off ? `Power ${d.name ?? d.id} (toggle)` : `Power Off ${d.name ?? d.id}`, device: dv, ha: d.kind === 'ha' });
    }
  }
  return { items, totalMs: items.reduce((n, i) => n + (i.kind === 'gap' ? i.ms : 0), 0) };
}
export const listFor = (when: 'start' | 'stop'): 'steps' | 'off' => (when === 'start' ? 'steps' : 'off');

export function addStep(cfg: Config, id: string, when: 'start' | 'stop', action: Action, at?: number): Config | { refused: string } {
  const a = activityOf(cfg, id);
  const list = listFor(when);
  if (!a) return { refused: 'No such activity' };
  if ((a[list] ?? []).length >= MAX_STEPS) return { refused: `At most ${MAX_STEPS} own steps` };
  if ('activity' in action) return { refused: 'A step cannot start an activity' };
  return edit(cfg, id, (x) => {
    const arr = [...(x[list] ?? [])];
    arr.splice(at ?? arr.length, 0, action);
    x[list] = arr;
  });
}
export const removeStep = (cfg: Config, id: string, when: 'start' | 'stop', index: number): Config =>
  edit(cfg, id, (a) => {
    const list = listFor(when);
    const arr = [...(a[list] ?? [])];
    arr.splice(index, 1);
    if (arr.length) a[list] = arr;
    else delete a[list];
  });
/** Reorder own steps (drag drop and Alt+arrows both end here). */
export function moveStep(cfg: Config, id: string, when: 'start' | 'stop', from: number, to: number): Config {
  const a = activityOf(cfg, id);
  const n = a?.[listFor(when)]?.length ?? 0;
  if (from === to || from < 0 || to < 0 || from >= n || to >= n) return cfg;
  return edit(cfg, id, (x) => {
    const arr = [...(x[listFor(when)] ?? [])];
    const [s] = arr.splice(from, 1);
    arr.splice(to, 0, s as Rec);
    x[listFor(when)] = arr;
  });
}

// ---------------------------------------------------------------- who handles it
export const ZONES = ['volume', 'navigation', 'transport', 'channels'] as const;
export type Zone = (typeof ZONES)[number];
export const ZONE_KEYS: Record<Zone, string[]> = {
  volume: ['volume_up', 'volume_down', 'volume_mute'],
  navigation: ['up', 'down', 'left', 'right', 'select', 'menu', 'exit'],
  transport: ['play', 'pause', 'stop', 'rewind', 'fast_forward', 'record'],
  channels: ['channel_up', 'channel_down', 'prev_channel'],
};
/** hard key -> command names it accepts, normalised (lower case, letters and digits, + / - spelled out, key/btn prefix dropped). */
export const MATCH: Record<string, string[]> = {
  volume_up: ['volup', 'volumeup', 'volplus', 'volumeplus', 'volinc', 'vup'],
  volume_down: ['voldown', 'volumedown', 'volminus', 'volumeminus', 'voldec', 'vdown', 'voldn'],
  volume_mute: ['mute', 'volmute', 'volumemute', 'muting'],
  up: ['up', 'cursorup', 'navup', 'dpadup'],
  down: ['down', 'cursordown', 'navdown', 'dpaddown'],
  left: ['left', 'cursorleft', 'navleft', 'dpadleft'],
  right: ['right', 'cursorright', 'navright', 'dpadright'],
  select: ['ok', 'select', 'enter', 'center', 'confirm'],
  menu: ['menu', 'topmenu', 'home', 'homemenu'],
  exit: ['exit', 'back', 'return', 'cancel'],
  play: ['play', 'playpause'],
  pause: ['pause', 'playpause'],
  stop: ['stop'],
  rewind: ['rew', 'rewind', 'backward', 'reverse', 'fastrewind'],
  fast_forward: ['ff', 'fastforward', 'forward', 'fwd'],
  record: ['record', 'rec'],
  channel_up: ['chup', 'channelup', 'chanup', 'chplus', 'channelplus', 'chanplus', 'programup', 'pageup'],
  channel_down: ['chdown', 'channeldown', 'chandown', 'chminus', 'channelminus', 'chanminus', 'programdown', 'pagedown'],
  prev_channel: ['prevch', 'prevchannel', 'previouschannel', 'lastchannel', 'last', 'recall', 'prechannel'],
};
export const normName = (n: string): string =>
  n.toLowerCase().replace(/\+/g, 'plus').replace(/-/g, 'minus').replace(/[^a-z0-9]/g, '').replace(/^(key|btn|button)(?=[a-z])/, '');

export interface Match {
  matched: Record<string, string>;
  missing: string[];
}
/** Which command of `d` answers each key of the zone: the first accepted spelling wins; exact match only. */
export function matchZone(d: Device, zone: Zone): Match {
  const by = new Map<string, string>();
  for (const n of commandNames(d)) if (!by.has(normName(n))) by.set(normName(n), n);
  const matched: Record<string, string> = {};
  const missing: string[] = [];
  for (const key of ZONE_KEYS[zone]) {
    const hit = (MATCH[key] ?? []).map((c) => by.get(c)).find((v) => v !== undefined);
    if (hit) matched[key] = hit;
    else missing.push(key);
  }
  return { matched, missing };
}

export interface HandlerResult {
  config: Config;
  matched: Record<string, string>;
  missing: string[];
  /** keys that already had their own action in this layer: left alone */
  kept: string[];
}
/** Make `device` handle `zone` in activity `id` (null clears it). Keys the user set by hand are never overwritten. */
export function applyHandler(cfg: Config, id: string, zone: Zone, device: string | null): HandlerResult {
  const next = clone(cfg);
  const act = (next.activities ?? []).find((a) => a.id === id);
  if (!act) return { config: cfg, matched: {}, missing: [], kept: [] };
  const meta = metaOf(next, id);
  const keys = (act.keys = act.keys ?? {});
  // take back what this zone wrote earlier, if the key still holds it
  for (const key of ZONE_KEYS[zone]) {
    const prev = meta.filled?.[key];
    if (prev && JSON.stringify(keys[key]) === JSON.stringify(prev)) delete keys[key];
    if (meta.filled) delete meta.filled[key];
  }
  const out: HandlerResult = { config: next, matched: {}, missing: [], kept: [] };
  meta.handlers = meta.handlers ?? {};
  if (device) {
    meta.handlers[zone] = device;
    const dev = (next.devices ?? []).find((d) => d.id === device);
    const m = dev ? matchZone(dev, zone) : { matched: {}, missing: [...ZONE_KEYS[zone]] };
    out.missing = m.missing;
    for (const [key, command] of Object.entries(m.matched)) {
      if (key in keys) {
        out.kept.push(key);
        continue;
      }
      const action = { device, command };
      keys[key] = action;
      meta.filled = { ...(meta.filled ?? {}), [key]: action };
      out.matched[key] = command;
    }
  } else delete meta.handlers[zone];
  if (!Object.keys(keys).length) delete act.keys;
  if (meta.filled && !Object.keys(meta.filled).length) delete meta.filled;
  if (!Object.keys(meta.handlers).length) delete meta.handlers;
  return out;
}
export const handlerOf = (cfg: Config, id: string, zone: Zone): string | null => readMeta(cfg, id).handlers?.[zone] ?? null;

// ---------------------------------------------------------------- auto-create
/** A start button for the activity on the Home page (or the first page): first free spot, widest that fits. */
export function addStartButton(cfg: Config, id: string): { config: Config; message: string } | { refused: string } {
  const pages = cfg.pages ?? [];
  const pi = Math.max(0, pages.findIndex((p) => p.kind === 'home'));
  const page = pages[pi];
  const act = activityOf(cfg, id);
  if (!page || !act) return { refused: 'There is no Home page to put the button on yet. Add a page in the Remote studio.' };
  const cols = pageDims(page).cols;
  for (const w of page.list ? [1] : [Math.min(cols, 4), Math.min(cols, 2), 1]) {
    const at = page.list ? { x: 0, y: 0 } : firstFree(page, w, 1);
    if (!at) continue;
    const next = clone(cfg);
    (next.pages as typeof pages)[pi]?.widgets.push({ type: 'button', id: uniqueWidgetId(next, 'btn'), ...(page.list ? {} : { x: at.x, y: at.y, w, h: 1 }), label: trimToBytes(act.name ?? act.id, LABEL_MAX), action: { activity: id } });
    return { config: next, message: `Added a "${act.name ?? act.id}" button to ${page.title || page.id}` };
  }
  return { refused: `${page.title || page.id} is full. Make room in the Remote studio first.` };
}
/** A control page from the "Activity control" template that shows while this activity runs. */
export function addControlPage(cfg: Config, id: string): Config {
  const first = chainOrder(cfg, id).find((d) => (cfg.devices ?? []).find((x) => x.id === d)?.commands) ?? undefined;
  const pages = templatePages(cfg, 'activity', { activity: id, ...(first ? { device: first } : {}) });
  return pages.reduce((c, page) => addPage(c, page), cfg);
}
