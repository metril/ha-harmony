// Client-side diff of the draft against the applied config, as Added / Changed / Removed rows.
import type { Config } from './model';

export interface DiffRow {
  kind: 'added' | 'changed' | 'removed';
  area: string;
  label: string;
  detail?: string;
}

const same = (a: unknown, b: unknown): boolean => JSON.stringify(a) === JSON.stringify(b);
type Rec = Record<string, unknown>;

function keyDiff(a: Rec | undefined, b: Rec | undefined) {
  const A = a ?? {};
  const B = b ?? {};
  const added = Object.keys(B).filter((k) => !(k in A));
  const removed = Object.keys(A).filter((k) => !(k in B));
  const changed = Object.keys(B).filter((k) => k in A && !same(A[k], B[k]));
  return { added, removed, changed };
}

function plural(n: number, w: string) {
  return `${n} ${w}${n === 1 ? '' : 's'}`;
}

function describe(a: Rec, b: Rec, groups: string[]): string {
  const parts: string[] = [];
  for (const g of groups) {
    const d = keyDiff(a[g] as Rec | undefined, b[g] as Rec | undefined);
    const bits = [
      d.added.length && `${d.added.length} added`,
      d.removed.length && `${d.removed.length} removed`,
      d.changed.length && `${d.changed.length} changed`,
    ].filter(Boolean);
    if (bits.length) parts.push(`${g}: ${bits.join(', ')}`);
  }
  for (const k of Object.keys({ ...a, ...b })) {
    if (groups.includes(k)) continue;
    if (!same(a[k], b[k])) parts.push(k === 'power' ? 'power changed' : `${k} changed`);
  }
  return parts.join('; ');
}

function byId(list: unknown): Map<string, Rec> {
  const m = new Map<string, Rec>();
  for (const x of Array.isArray(list) ? (list as Rec[]) : []) m.set(String(x.id), x);
  return m;
}

export function diffConfig(applied: Config | null, draft: Config | null): DiffRow[] {
  const rows: DiffRow[] = [];
  const A = (applied ?? {}) as Rec;
  const B = (draft ?? {}) as Rec;
  const areas: [string, string, string[]][] = [
    ['devices', 'Device', ['commands', 'inputs']],
    ['activities', 'Activity', ['keys']],
    ['pages', 'Page', []],
  ];
  for (const [key, area, groups] of areas) {
    const a = byId(A[key]);
    const b = byId(B[key]);
    for (const [id, x] of b) {
      const label = String(x.name ?? id);
      const old = a.get(id);
      if (!old) {
        const n = key === 'devices' ? Object.keys((x.commands as Rec) ?? {}).length : 0;
        rows.push({ kind: 'added', area, label, detail: n ? plural(n, 'command') : undefined });
      } else if (!same(old, x)) {
        const detail = key === 'pages' ? 'layout changed' : describe(old, x, groups);
        rows.push({ kind: 'changed', area, label, detail });
      }
    }
    for (const [id, x] of a) if (!b.has(id)) rows.push({ kind: 'removed', area, label: String(x.name ?? id) });
  }
  if (!same(A['theme'] ?? 'default', B['theme'] ?? 'default')) rows.push({ kind: 'changed', area: 'Active Theme', label: String(B['theme'] ?? 'default'), detail: `was ${String(A['theme'] ?? 'default')}` });
  const ta = (A['themes'] ?? {}) as Rec;
  const tb = (B['themes'] ?? {}) as Rec;
  for (const n of Object.keys(tb)) if (!(n in ta)) rows.push({ kind: 'added', area: 'Theme', label: n, detail: 'new' });
  for (const n of Object.keys(tb)) if (n in ta && !same(ta[n], tb[n])) rows.push({ kind: 'changed', area: 'Theme', label: n, detail: 'colours changed' });
  for (const n of Object.keys(ta)) if (!(n in tb)) rows.push({ kind: 'removed', area: 'Theme', label: n });
  const k = keyDiff(A.keys as Rec | undefined, B.keys as Rec | undefined);
  for (const n of k.added) rows.push({ kind: 'added', area: 'Hard Key', label: n });
  for (const n of k.changed) rows.push({ kind: 'changed', area: 'Hard Key', label: n });
  for (const n of k.removed) rows.push({ kind: 'removed', area: 'Hard Key', label: n });
  const other = new Set([...Object.keys(A), ...Object.keys(B)]);
  for (const n of ['devices', 'activities', 'pages', 'keys', 'theme', 'themes']) other.delete(n);
  for (const n of other) if (!same(A[n], B[n])) rows.push({ kind: 'changed', area: 'Setting', label: n });
  return rows;
}

export interface KeyChange {
  /** `default` or an activity id */
  layer: string;
  layerName: string;
  key: string;
  kind: 'added' | 'changed' | 'removed';
}
/** Hard keys that differ between two configs, per layer (Default and each activity). */
export function changedKeys(applied: Config | null, draft: Config | null): KeyChange[] {
  const out: KeyChange[] = [];
  const layers = (c: Config | null): Map<string, { name: string; keys: Rec }> => {
    const m = new Map<string, { name: string; keys: Rec }>([['default', { name: 'Default', keys: (c?.keys ?? {}) as Rec }]]);
    for (const a of c?.activities ?? []) m.set(a.id, { name: a.name ?? a.id, keys: (a.keys ?? {}) as Rec });
    return m;
  };
  const A = layers(applied);
  const B = layers(draft);
  for (const [id, b] of B) {
    const d = keyDiff(A.get(id)?.keys, b.keys);
    for (const key of d.added) out.push({ layer: id, layerName: b.name, key, kind: 'added' });
    for (const key of d.changed) out.push({ layer: id, layerName: b.name, key, kind: 'changed' });
    for (const key of d.removed) out.push({ layer: id, layerName: b.name, key, kind: 'removed' });
  }
  return out;
}

export interface PageChange {
  id: string;
  title: string;
  kind: 'added' | 'changed' | 'removed';
  /** labels of the widgets that were added, changed or removed (by widget id) */
  widgets: string[];
}
/** Indexes (in each page's own widget list) of the widgets that were added, changed or removed, matched by widget id. */
export function changedWidgetIndexes(a: Rec | undefined, b: Rec | undefined): { before: number[]; after: number[] } {
  const wa = ((a?.['widgets'] as Rec[]) ?? []);
  const wb = ((b?.['widgets'] as Rec[]) ?? []);
  const key = (w: Rec, i: number) => String(w['id'] ?? `#${i}`);
  const ma = new Map(wa.map((w, i) => [key(w, i), w]));
  const mb = new Map(wb.map((w, i) => [key(w, i), w]));
  return {
    before: wa.flatMap((w, i) => (same(w, mb.get(key(w, i))) ? [] : [i])),
    after: wb.flatMap((w, i) => (same(w, ma.get(key(w, i))) ? [] : [i])),
  };
}
/** Pages that differ, with the widgets that changed on them. */
export function changedPages(applied: Config | null, draft: Config | null): PageChange[] {
  const out: PageChange[] = [];
  const pa = byId(applied?.pages);
  const pb = byId(draft?.pages);
  for (const [id, b] of pb) {
    const a = pa.get(id);
    const title = String(b['title'] ?? id);
    if (!a) out.push({ id, title, kind: 'added', widgets: [] });
    else if (!same(a, b)) {
      const wa = new Map(((a['widgets'] as Rec[]) ?? []).map((w, i) => [String(w['id'] ?? `#${i}`), w]));
      const labels: string[] = [];
      ((b['widgets'] as Rec[]) ?? []).forEach((w, i) => {
        const k = String(w['id'] ?? `#${i}`);
        if (!same(wa.get(k), w)) labels.push(String(w['label'] ?? k));
        wa.delete(k);
      });
      for (const w of wa.values()) labels.push(String(w['label'] ?? w['id']));
      out.push({ id, title, kind: 'changed', widgets: labels });
    }
  }
  for (const [id, a] of pa) if (!pb.has(id)) out.push({ id, title: String(a['title'] ?? id), kind: 'removed', widgets: [] });
  return out;
}
