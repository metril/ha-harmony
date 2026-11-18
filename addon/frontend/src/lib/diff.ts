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
  const k = keyDiff(A.keys as Rec | undefined, B.keys as Rec | undefined);
  for (const n of k.added) rows.push({ kind: 'added', area: 'Hard key', label: n });
  for (const n of k.changed) rows.push({ kind: 'changed', area: 'Hard key', label: n });
  for (const n of k.removed) rows.push({ kind: 'removed', area: 'Hard key', label: n });
  const other = new Set([...Object.keys(A), ...Object.keys(B)]);
  for (const n of ['devices', 'activities', 'pages', 'keys']) other.delete(n);
  for (const n of other) if (!same(A[n], B[n])) rows.push({ kind: 'changed', area: 'Setting', label: n });
  return rows;
}
