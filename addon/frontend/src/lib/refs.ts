// Reference index: where a device, command or input is used (hard keys, widgets, activity steps and
// inputs, "who handles it"). Drives the delete guard; rename and delete keep every reference in step.
import type { Activity, Config, Device } from './model';

export type Ref =
  | { kind: 'key'; layer: string; layerName: string; key: string }
  | { kind: 'widget'; page: number; pageName: string; index: number; label: string }
  | { kind: 'step'; activity: string; activityName: string; list: 'steps' | 'off'; index: number }
  | { kind: 'activity'; activity: string; activityName: string; input?: string }
  | { kind: 'handler'; activity: string; activityName: string; zone: string };

type Rec = Record<string, unknown>;
export type Target = { device: string; command?: string; input?: string };

/** The action as seen by a reference check: `tap` and `action` both hold actions. */
function matches(a: unknown, t: Target): boolean {
  if (!a || typeof a !== 'object') return false;
  const x = a as Rec;
  if (x['device'] !== t.device) return false;
  if (t.command !== undefined) return x['command'] === t.command;
  if (t.input !== undefined) return x['input'] === t.input;
  return true;
}

export function findRefs(cfg: Config, t: Target): Ref[] {
  const out: Ref[] = [];
  for (const [key, a] of Object.entries(cfg.keys ?? {})) if (matches(a, t)) out.push({ kind: 'key', layer: 'default', layerName: 'Default', key });
  for (const act of cfg.activities ?? []) {
    const an = act.name ?? act.id;
    for (const [key, a] of Object.entries(act.keys ?? {})) if (matches(a, t)) out.push({ kind: 'key', layer: act.id, layerName: an, key });
    for (const list of ['steps', 'off'] as const) (act[list] ?? []).forEach((a, index) => matches(a, t) && out.push({ kind: 'step', activity: act.id, activityName: an, list, index }));
    for (const d of act.devices ?? []) {
      if (d.device !== t.device) continue;
      if (t.command !== undefined) continue;
      if (t.input === undefined || d.input === t.input) out.push({ kind: 'activity', activity: act.id, activityName: an, ...(d.input ? { input: d.input } : {}) });
    }
    const handlers = ((cfg.editor?.['activities'] as Rec | undefined)?.[act.id] as { handlers?: Record<string, string> } | undefined)?.handlers ?? {};
    if (t.command === undefined && t.input === undefined)
      for (const [zone, dev] of Object.entries(handlers)) if (dev === t.device) out.push({ kind: 'handler', activity: act.id, activityName: an, zone });
  }
  (cfg.pages ?? []).forEach((p, page) =>
    p.widgets.forEach((w, index) => {
      if (matches(w['action'], t) || matches(w['tap'], t)) out.push({ kind: 'widget', page, pageName: p.title || p.id, index, label: w.label ?? w.id ?? w.type });
    }),
  );
  return out;
}

/** The screen that shows this use: the studio (selecting the key or widget) or the activity. */
export function refHref(r: Ref): string {
  switch (r.kind) {
    case 'key':
      return `#/remote/${encodeURIComponent(r.layer === 'default' ? 'default' : r.layer)}/${encodeURIComponent(r.key)}`;
    case 'widget':
      return `#/remote/default/${encodeURIComponent(`w${r.page}.${r.index}`)}`;
    default:
      return `#/activities/${encodeURIComponent(r.activity)}`;
  }
}

export function describeRef(r: Ref): string {
  switch (r.kind) {
    case 'key':
      return `Key ${r.key.replace(/_/g, ' ')} (${r.layerName} Layer)`;
    case 'widget':
      return `Widget "${r.label}" on page ${r.pageName}`;
    case 'step':
      return `${r.list === 'steps' ? 'Start' : 'Stop'} step ${r.index + 1} of ${r.activityName}`;
    case 'activity':
      return `Activity ${r.activityName}${r.input ? ` (input ${r.input})` : ''}`;
    case 'handler':
      return `${r.activityName} handles ${r.zone} with this device`;
  }
}

const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;

/** Visit every action holder (key layers, steps, widgets) with a setter that rewrites or removes it. */
function walk(cfg: Config, t: Target, fn: (a: Rec) => Rec | null | undefined): void {
  const fix = (holder: Rec, k: string, removeOnNull: boolean) => {
    const a = holder[k];
    if (!matches(a, t)) return;
    const r = fn(a as Rec);
    if (r === undefined) return;
    if (r === null) {
      if (removeOnNull) delete holder[k];
      else holder[k] = { none: true };
    } else holder[k] = r;
  };
  for (const k of Object.keys(cfg.keys ?? {})) fix(cfg.keys as Rec, k, true);
  for (const act of cfg.activities ?? []) {
    for (const k of Object.keys(act.keys ?? {})) fix(act.keys as Rec, k, true);
    for (const list of ['steps', 'off'] as const) {
      const arr = act[list];
      if (!arr) continue;
      for (let i = arr.length - 1; i >= 0; i--) {
        if (!matches(arr[i], t)) continue;
        const r = fn(arr[i] as Rec);
        if (r === null) arr.splice(i, 1);
        else if (r) arr[i] = r;
      }
    }
  }
  for (const p of cfg.pages ?? []) {
    for (const w of p.widgets) {
      fix(w, 'action', false);
      fix(w, 'tap', false);
    }
  }
}

function tidyActivities(cfg: Config) {
  for (const act of cfg.activities ?? []) {
    if (act.keys && !Object.keys(act.keys).length) delete act.keys;
  }
}

/** Rename a command or input everywhere it is used. */
export function renameItem(cfg: Config, device: string, kind: 'command' | 'input', from: string, to: string): Config {
  const next = clone(cfg);
  const d = (next.devices ?? []).find((x) => x.id === device);
  if (!d) return next;
  const field = kind === 'command' ? 'commands' : 'inputs';
  const src = d[field];
  if (!src || !(from in src) || from === to) return next;
  d[field] = Object.fromEntries(Object.entries(src).map(([k, v]) => [k === from ? to : k, v]));
  const t: Target = kind === 'command' ? { device, command: from } : { device, input: from };
  walk(next, t, (a) => ({ ...a, [kind]: to }));
  if (kind === 'input') for (const act of next.activities ?? []) for (const dv of act.devices ?? []) if (dv.device === device && dv.input === from) dv.input = to;
  return next;
}

/** Delete a command or input and everything that pointed at it (keys cleared, widgets set to "no action", steps removed). */
export function deleteItem(cfg: Config, device: string, kind: 'command' | 'input', name: string): Config {
  const next = clone(cfg);
  const d = (next.devices ?? []).find((x) => x.id === device);
  const field = kind === 'command' ? 'commands' : 'inputs';
  if (d?.[field]) {
    delete d[field][name];
  }
  walk(next, kind === 'command' ? { device, command: name } : { device, input: name }, () => null);
  if (kind === 'input') for (const act of next.activities ?? []) for (const dv of act.devices ?? []) if (dv.device === device && dv.input === name) delete dv.input;
  tidyActivities(next);
  return next;
}

/** Remove a device with all its references (also drops it from activities and "who handles it"). */
export function deleteDevice(cfg: Config, device: string): Config {
  const next = clone(cfg);
  next.devices = (next.devices ?? []).filter((d: Device) => d.id !== device);
  walk(next, { device }, () => null);
  for (const act of next.activities ?? []) {
    act.devices = (act.devices ?? []).filter((d) => d.device !== device);
    const meta = (next.editor?.['activities'] as Record<string, { handlers?: Record<string, string>; chain?: string[]; filled?: Rec }> | undefined)?.[act.id];
    if (meta) {
      if (meta.handlers) for (const z of Object.keys(meta.handlers)) if (meta.handlers[z] === device) delete meta.handlers[z];
      if (meta.chain) meta.chain = meta.chain.filter((x) => x !== device);
    }
  }
  tidyActivities(next);
  return next;
}

export const activityOf = (cfg: Config, id: string): Activity | undefined => (cfg.activities ?? []).find((a) => a.id === id);
