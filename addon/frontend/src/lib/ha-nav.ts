// Navigation logic for the Home Assistant entity browser: search, filters, grouping, remembered state.
// Pure functions only (no DOM), so the same code serves the palette, the action picker and the wizard.
import type { HaEntity } from './api';

export type GroupBy = 'area' | 'type' | 'device' | 'integration' | 'label';
export const GROUP_BY: { id: GroupBy; label: string }[] = [
  { id: 'area', label: 'Area' },
  { id: 'type', label: 'Type' },
  { id: 'device', label: 'Device' },
  { id: 'integration', label: 'Integration' },
  { id: 'label', label: 'Label' },
];

/** The type chips: a few domains each, in the order they are shown. */
export const TYPE_CHIPS: { id: string; label: string; domains: string[] }[] = [
  { id: 'light', label: 'Lights', domains: ['light'] },
  { id: 'switch', label: 'Switches', domains: ['switch'] },
  { id: 'scene', label: 'Scenes', domains: ['scene'] },
  { id: 'script', label: 'Scripts', domains: ['script'] },
  { id: 'media_player', label: 'Media players', domains: ['media_player', 'remote'] },
  { id: 'climate', label: 'Climate', domains: ['climate'] },
  { id: 'cover', label: 'Covers', domains: ['cover'] },
  { id: 'fan', label: 'Fans', domains: ['fan'] },
  { id: 'input', label: 'Buttons and inputs', domains: ['button', 'input_button', 'input_boolean', 'number', 'select'] },
  { id: 'sensor', label: 'Sensors', domains: ['sensor', 'binary_sensor'] },
];
const TYPE_OF = new Map(TYPE_CHIPS.flatMap((t) => t.domains.map((d) => [d, t.id] as const)));
const TYPE_LABEL = new Map(TYPE_CHIPS.map((t) => [t.id, t.label]));
const TYPE_ORDER = new Map(TYPE_CHIPS.map((t, i) => [t.id, i]));

export const domainOf = (e: HaEntity): string => e.domain ?? e.entity_id.split('.')[0] ?? '';
export const typeOf = (e: HaEntity): string => TYPE_OF.get(domainOf(e)) ?? domainOf(e);
export const typeLabel = (id: string): string => TYPE_LABEL.get(id) ?? id.replace(/_/g, ' ');

export const NO_AREA = 'No area';
export const NO_FLOOR = 'No floor';
export const NO_DEVICE = 'No device';
export const NO_INTEGRATION = 'No integration';
export const NO_LABEL = 'No label';
/** a group with more rows than this is split once more (by type, or by area for a type group) */
export const SUBGROUP_MIN = 12;

export interface Filters {
  q: string;
  types: ReadonlySet<string>;
  areas: ReadonlySet<string>;
  labels: ReadonlySet<string>;
  integrations: ReadonlySet<string>;
}
export const emptyFilters = (): Filters => ({ q: '', types: new Set(), areas: new Set(), labels: new Set(), integrations: new Set() });
/** how many of the chip / dropdown filters are on (the search text is not counted) */
export const filterCount = (f: Filters): number => f.types.size + f.areas.size + f.labels.size + f.integrations.size;

export const queryWords = (q: string): string[] => q.toLowerCase().split(/\s+/).filter(Boolean);

const HAY = new WeakMap<HaEntity, string>();
/** lower-cased text a search matches: name, entity id, area, device, integration, labels */
function hay(e: HaEntity): string {
  let h = HAY.get(e);
  if (h === undefined) {
    h = [e.name, e.entity_id, e.area, e.device, e.platform, ...(e.labels ?? [])].filter(Boolean).join(' ').toLowerCase();
    HAY.set(e, h);
  }
  return h;
}
export const matchWords = (e: HaEntity, words: readonly string[]): boolean => {
  const h = hay(e);
  return words.every((w) => h.includes(w));
};

export type Dim = 'types' | 'areas' | 'labels' | 'integrations';
/** entities that pass the search and every filter except the one named in `skip` (for facet counts) */
export function filterEntities(list: readonly HaEntity[], f: Filters, skip?: Dim): HaEntity[] {
  const words = queryWords(f.q);
  const on = (d: Dim) => d !== skip && f[d].size > 0;
  return list.filter(
    (e) =>
      (!on('types') || f.types.has(typeOf(e))) &&
      (!on('areas') || f.areas.has(e.area ?? NO_AREA)) &&
      (!on('integrations') || f.integrations.has(e.platform ?? NO_INTEGRATION)) &&
      (!on('labels') || (e.labels?.length ? e.labels.some((l) => f.labels.has(l)) : f.labels.has(NO_LABEL))) &&
      (!words.length || matchWords(e, words)),
  );
}

/** counts of one facet over the entities that pass everything else */
export function facet(list: readonly HaEntity[], f: Filters, dim: Dim): Map<string, number> {
  const out = new Map<string, number>();
  const add = (k: string) => out.set(k, (out.get(k) ?? 0) + 1);
  for (const e of filterEntities(list, f, dim)) {
    if (dim === 'types') add(typeOf(e));
    else if (dim === 'areas') add(e.area ?? NO_AREA);
    else if (dim === 'integrations') add(e.platform ?? NO_INTEGRATION);
    else if (e.labels?.length) e.labels.forEach(add);
    else add(NO_LABEL);
  }
  return out;
}

/** which group-by options have data: area needs registries with an area, label needs a label, ... */
export function availableGroupBys(list: readonly HaEntity[], registries: boolean): GroupBy[] {
  const has = {
    area: registries && list.some((e) => e.area),
    device: registries && list.some((e) => e.device),
    integration: registries && list.some((e) => e.platform),
    label: registries && list.some((e) => e.labels?.length),
  };
  return GROUP_BY.map((g) => g.id).filter((id) => id === 'type' || has[id]);
}

export interface Group {
  /** stable id, also the key of the remembered open state: "area:Ground/Kitchen" */
  id: string;
  title: string;
  /** small second line (the floor of an area, the entity count of a device ...) */
  sub?: string;
  kind: 'floor' | 'area' | 'type' | 'device' | 'integration' | 'label';
  count: number;
  /** rows, for a leaf group */
  items?: HaEntity[];
  /** sub-groups (floor -> areas; a big group -> by type or area) */
  children?: Group[];
  /** the type id of a type group (its icon) */
  type?: string;
}

const byName = (a: HaEntity, b: HaEntity) => a.name?.toLowerCase().localeCompare(b.name?.toLowerCase() ?? '') || a.entity_id.localeCompare(b.entity_id);
const byTitle = (none: string) => (a: Group, b: Group) => (a.title === none ? 1 : b.title === none ? -1 : a.title.localeCompare(b.title, undefined, { sensitivity: 'base', numeric: true }));

function bucket(list: readonly HaEntity[], keys: (e: HaEntity) => string[]): Map<string, HaEntity[]> {
  const m = new Map<string, HaEntity[]>();
  for (const e of list)
    for (const k of keys(e)) {
      const a = m.get(k);
      if (a) a.push(e);
      else m.set(k, [e]);
    }
  return m;
}

type Ctx = { floorOf: ReadonlyMap<string, string> };
const leaf = (by: GroupBy, prefix: string, title: string, items: HaEntity[], extra: Partial<Group> = {}): Group => ({
  id: `${prefix}${title}`,
  title,
  kind: by === 'type' ? 'type' : by === 'area' ? 'area' : by,
  count: items.length,
  items: items.sort(byName),
  ...extra,
});

/** Split a big leaf group once more: Area/Device/Integration/Label by type, Type by area. */
function subgroup(g: Group, by: GroupBy): Group {
  if (!g.items || g.items.length <= SUBGROUP_MIN) return g;
  const sub: GroupBy = by === 'type' ? 'area' : 'type';
  const m = bucket(g.items, (e) => [sub === 'type' ? typeOf(e) : (e.area ?? NO_AREA)]);
  if (m.size < 2) return g;
  const kids = [...m].map(([k, items]) => (sub === 'type' ? leaf('type', `${g.id}/`, k, items, { title: typeLabel(k), type: k, id: `${g.id}/${k}` }) : leaf('area', `${g.id}/`, k, items)));
  kids.sort(sub === 'type' ? (a, b) => (TYPE_ORDER.get(a.type ?? '') ?? 99) - (TYPE_ORDER.get(b.type ?? '') ?? 99) || a.title.localeCompare(b.title) : byTitle(NO_AREA));
  return { ...g, items: undefined, children: kids };
}

export function groupEntities(list: readonly HaEntity[], by: GroupBy, ctx: Ctx = { floorOf: new Map() }): Group[] {
  const p = `${by}:`;
  if (by === 'type') {
    const m = bucket(list, (e) => [typeOf(e)]);
    return [...m]
      .map(([k, items]) => subgroup(leaf('type', p, k, items, { title: typeLabel(k), type: k, id: `${p}${k}` }), by))
      .sort((a, b) => (TYPE_ORDER.get(a.type ?? '') ?? 99) - (TYPE_ORDER.get(b.type ?? '') ?? 99) || a.title.localeCompare(b.title));
  }
  if (by === 'area') {
    const m = bucket(list, (e) => [e.area ?? NO_AREA]);
    const floors = new Map<string, Group[]>();
    const flat: Group[] = [];
    for (const [k, items] of m) {
      const floor = ctx.floorOf.get(k);
      const g = subgroup(leaf('area', floor ? `${p}${floor}/` : p, k, items, floor ? { sub: floor } : {}), by);
      const fl = floor && k !== NO_AREA ? floors.get(floor) : undefined;
      if (floor && k !== NO_AREA && fl) fl.push(g);
      else if (floor && k !== NO_AREA) floors.set(floor, [g]);
      else flat.push(g);
    }
    if (!floors.size) return flat.sort(byTitle(NO_AREA));
    const out: Group[] = [...floors].map(([floor, kids]) => ({
      id: `${p}${floor}`,
      title: floor,
      kind: 'floor' as const,
      count: kids.reduce((n, k) => n + k.count, 0),
      children: kids.sort(byTitle(NO_AREA)),
    }));
    out.sort(byTitle(NO_FLOOR));
    const loose = flat.filter((g) => g.title !== NO_AREA);
    const none = flat.filter((g) => g.title === NO_AREA);
    if (loose.length) out.push({ id: `${p}${NO_FLOOR}`, title: NO_FLOOR, kind: 'floor', count: loose.reduce((n, k) => n + k.count, 0), children: loose.sort(byTitle(NO_AREA)) });
    return [...out, ...none];
  }
  const none = by === 'device' ? NO_DEVICE : by === 'integration' ? NO_INTEGRATION : NO_LABEL;
  const keys = (e: HaEntity): string[] => (by === 'device' ? [e.device ?? none] : by === 'integration' ? [e.platform ?? none] : e.labels?.length ? e.labels : [none]);
  return [...bucket(list, keys)].map(([k, items]) => subgroup(leaf(by, p, k, items), by)).sort(byTitle(none));
}

export const groupIds = (gs: readonly Group[]): string[] => gs.flatMap((g) => [g.id, ...(g.children ? groupIds(g.children) : [])]);

/** Split `text` into pieces, marking those a search word matches (for highlighting). */
export function highlight(text: string, words: readonly string[]): { t: string; m: boolean }[] {
  if (!words.length) return [{ t: text, m: false }];
  const low = text.toLowerCase();
  const mark = new Array<boolean>(text.length).fill(false);
  for (const w of words) for (let i = low.indexOf(w); i >= 0; i = low.indexOf(w, i + w.length)) mark.fill(true, i, i + w.length);
  const out: { t: string; m: boolean }[] = [];
  for (let i = 0; i < text.length; i++) {
    const last = out[out.length - 1];
    if (last && last.m === mark[i]) last.t += text[i];
    else out.push({ t: text[i] as string, m: mark[i] as boolean });
  }
  return out;
}

/** The sentence under "Home Assistant cannot be read", from the 503's `reason`. */
export function reasonText(reason: string | null | undefined): string {
  const r = reason ?? '';
  if (r === 'no_access') return 'The add-on has no access token for Home Assistant (it is not running under the Supervisor).';
  if (/^status_(401|403)$/.test(r)) return 'Home Assistant refused the add-on’s access token.';
  const m = /^status_(\d+)$/.exec(r);
  if (m) return `Home Assistant answered with an error (HTTP ${m[1]}).`;
  if (r === 'too_large') return 'Home Assistant’s answer was larger than the add-on accepts.';
  if (r === 'not_json' || r === 'unexpected_shape') return 'Home Assistant’s answer could not be understood.';
  if (r === 'TimeoutError') return 'Home Assistant took too long to answer.';
  if (r) return /Error$|Exception$/.test(r) ? 'The add-on could not connect to Home Assistant.' : `Reason: ${r}.`;
  return 'The add-on could not reach Home Assistant.';
}

// ---------------------------------------------------------------- remembered per browser
const LS = 'hh.ha.';
export function readList(key: string): string[] {
  try {
    const v: unknown = JSON.parse(localStorage.getItem(LS + key) ?? '[]');
    return Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string').slice(0, 2000) : [];
  } catch {
    return [];
  }
}
export function writeList(key: string, v: readonly string[]): void {
  try {
    localStorage.setItem(LS + key, JSON.stringify(v.slice(0, 2000)));
  } catch {
    /* private mode or storage full: the state is just not remembered */
  }
}
export function readText(key: string, fallback: string): string {
  try {
    return localStorage.getItem(LS + key) ?? fallback;
  } catch {
    return fallback;
  }
}
export function writeText(key: string, v: string): void {
  try {
    localStorage.setItem(LS + key, v);
  } catch {
    /* not remembered */
  }
}
export const RECENT_MAX = 8;
export const pushRecent = (list: readonly string[], id: string): string[] => [id, ...list.filter((x) => x !== id)].slice(0, RECENT_MAX);
export const toggled = (list: readonly string[], id: string): string[] => (list.includes(id) ? list.filter((x) => x !== id) : [...list, id]);
