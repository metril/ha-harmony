// Navigation logic for the Home Assistant entity browser: search, filters, grouping, remembered state.
// Pure functions only (no DOM), so the same code serves the palette, the action picker and the wizard.
import type { HaCatalog, HaEntity } from './api';

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
  { id: 'media_player', label: 'Media Players', domains: ['media_player', 'remote'] },
  { id: 'climate', label: 'Climate', domains: ['climate'] },
  { id: 'cover', label: 'Covers', domains: ['cover'] },
  { id: 'fan', label: 'Fans', domains: ['fan'] },
  { id: 'input', label: 'Buttons and Inputs', domains: ['button', 'input_button', 'input_boolean', 'number', 'select'] },
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
/** the key of "no area / device / label / integration" in filters and group ids */
export const NONE = '~none';
/** a group with more rows than this is split once more (by type, or by area for a type group) */
export const SUBGROUP_MIN = 12;

export interface AreaInfo {
  name: string;
  /** the name, plus the floor when two areas share it ("Hall · Up") */
  label: string;
  floor_id?: string;
  floor?: string;
}
/** Names for the ids the catalog's rows carry. Ids are the keys everywhere; names are for display. */
export interface Lookups {
  areas: ReadonlyMap<string, AreaInfo>;
  devices: ReadonlyMap<string, string>;
  labels: ReadonlyMap<string, string>;
}
export const noLookups: Lookups = { areas: new Map(), devices: new Map(), labels: new Map() };

/** Fill in the display fields (area, device and label names, search text) of the rows of a catalog. */
export function enrich(cat: Pick<HaCatalog, 'entities' | 'areas' | 'devices' | 'labels'>): { entities: HaEntity[]; lookups: Lookups } {
  const counts = new Map<string, number>();
  for (const a of cat.areas ?? []) counts.set(a.name, (counts.get(a.name) ?? 0) + 1);
  const areas = new Map<string, AreaInfo>();
  for (const a of cat.areas ?? [])
    areas.set(a.id, { name: a.name, label: (counts.get(a.name) ?? 0) > 1 ? `${a.name} · ${a.floor ?? a.id}` : a.name, ...(a.floor_id ? { floor_id: a.floor_id } : {}), ...(a.floor ? { floor: a.floor } : {}) });
  const devices = new Map((cat.devices ?? []).map((d) => [d.id, d.name]));
  const labels = new Map((cat.labels ?? []).map((l) => [l.id, l.name]));
  for (const e of cat.entities) {
    const a = e.area_id ? areas.get(e.area_id) : undefined;
    if (a) e.area = a.name;
    if (a?.floor_id) e.floor_id = a.floor_id;
    const d = e.device_id ? devices.get(e.device_id) : undefined;
    if (d) e.device = d;
    const l = (e.label_ids ?? []).map((i) => labels.get(i)).filter((x): x is string => !!x);
    if (l.length) e.labels = l;
  }
  return { entities: cat.entities, lookups: { areas, devices, labels } };
}

export interface Filters {
  q: string;
  types: ReadonlySet<string>;
  /** area ids (or NONE) */
  areas: ReadonlySet<string>;
  /** label ids (or NONE) */
  labels: ReadonlySet<string>;
  /** integration domains (or NONE) */
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
/** the filter keys of an entity along one dimension */
const keysOf = (e: HaEntity, d: Dim): string[] => (d === 'types' ? [typeOf(e)] : d === 'areas' ? [e.area_id ?? NONE] : d === 'integrations' ? [e.platform ?? NONE] : e.label_ids?.length ? e.label_ids : [NONE]);

/** entities that pass the search and every filter except the one named in `skip` (for facet counts) */
export function filterEntities(list: readonly HaEntity[], f: Filters, skip?: Dim): HaEntity[] {
  const words = queryWords(f.q);
  const dims = (['types', 'areas', 'labels', 'integrations'] as const).filter((d) => d !== skip && f[d].size > 0);
  return list.filter((e) => dims.every((d) => keysOf(e, d).some((k) => f[d].has(k))) && (!words.length || matchWords(e, words)));
}

/** counts of one facet over the entities that pass everything else */
export function facet(list: readonly HaEntity[], f: Filters, dim: Dim): Map<string, number> {
  const out = new Map<string, number>();
  for (const e of filterEntities(list, f, dim)) for (const k of keysOf(e, dim)) out.set(k, (out.get(k) ?? 0) + 1);
  return out;
}

/** which group-by options have data: area needs registries with an area, label needs a label, ... */
export function availableGroupBys(list: readonly HaEntity[], registries: boolean): GroupBy[] {
  const has = {
    area: registries && list.some((e) => e.area_id),
    device: registries && list.some((e) => e.device_id),
    integration: registries && list.some((e) => e.platform),
    label: registries && list.some((e) => e.label_ids?.length),
  };
  return GROUP_BY.map((g) => g.id).filter((id) => id === 'type' || has[id]);
}

export interface Group {
  /** stable id, also the key of the remembered open state: "floor:f1/area:kitchen" */
  id: string;
  title: string;
  /** small second line (the floor of an area) */
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
const byTitle = (a: Group, b: Group) => (a.id.endsWith(NONE) ? 1 : b.id.endsWith(NONE) ? -1 : a.title.localeCompare(b.title, undefined, { sensitivity: 'base', numeric: true }));
const byType = (a: Group, b: Group) => (TYPE_ORDER.get(a.type ?? '') ?? 99) - (TYPE_ORDER.get(b.type ?? '') ?? 99) || a.title.localeCompare(b.title);
/** one id segment: ids come from Home Assistant, so they are escaped rather than trusted */
const seg = (kind: string, v: string) => `${kind}:${encodeURIComponent(v)}`;

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

const leaf = (kind: Group['kind'], id: string, title: string, items: HaEntity[], extra: Partial<Group> = {}): Group => ({ id, title, kind, count: items.length, items: items.sort(byName), ...extra });

function areaTitle(k: string, lk: Lookups): string {
  return k === NONE ? NO_AREA : (lk.areas.get(k)?.label ?? k);
}

/** Split a big leaf group once more: Area/Device/Integration/Label by type, Type by area. */
function subgroup(g: Group, by: GroupBy, lk: Lookups): Group {
  if (!g.items || g.items.length <= SUBGROUP_MIN) return g;
  const byArea = by === 'type';
  const m = bucket(g.items, (e) => [byArea ? (e.area_id ?? NONE) : typeOf(e)]);
  if (m.size < 2) return g;
  const kids = [...m].map(([k, items]) => (byArea ? leaf('area', `${g.id}/${seg('area', k)}`, areaTitle(k, lk), items) : leaf('type', `${g.id}/${seg('type', k)}`, typeLabel(k), items, { type: k })));
  kids.sort(byArea ? byTitle : byType);
  return { ...g, items: undefined, children: kids };
}

export function groupEntities(list: readonly HaEntity[], by: GroupBy, lk: Lookups = noLookups): Group[] {
  if (by === 'type') {
    return [...bucket(list, (e) => [typeOf(e)])].map(([k, items]) => subgroup(leaf('type', seg('type', k), typeLabel(k), items, { type: k }), by, lk)).sort(byType);
  }
  if (by === 'area') {
    const floors = new Map<string, { title: string; kids: Group[] }>();
    const flat: Group[] = [];
    for (const [k, items] of bucket(list, (e) => [e.area_id ?? NONE])) {
      const info = k === NONE ? undefined : lk.areas.get(k);
      const fid = info?.floor_id;
      const g = subgroup(leaf('area', fid ? `${seg('floor', fid)}/${seg('area', k)}` : seg('area', k), areaTitle(k, lk), items, info?.floor ? { sub: info.floor } : {}), by, lk);
      const bucketOf = floors.get(fid ?? '');
      if (fid && bucketOf) bucketOf.kids.push(g);
      else if (fid) floors.set(fid, { title: info?.floor ?? fid, kids: [g] });
      else flat.push(g);
    }
    if (!floors.size) return flat.sort(byTitle);
    const out: Group[] = [...floors].map(([fid, { title, kids }]) => ({
      id: seg('floor', fid),
      title,
      kind: 'floor' as const,
      count: kids.reduce((n, k) => n + k.count, 0),
      children: kids.sort(byTitle),
    }));
    out.sort(byTitle);
    const loose = flat.filter((g) => !g.id.endsWith(NONE));
    const none = flat.filter((g) => g.id.endsWith(NONE));
    if (loose.length) out.push({ id: seg('floor', NONE), title: NO_FLOOR, kind: 'floor', count: loose.reduce((n, k) => n + k.count, 0), children: loose.sort(byTitle) });
    return [...out.sort((a, b) => (a.id.endsWith(NONE) ? 1 : b.id.endsWith(NONE) ? -1 : 0)), ...none];
  }
  const none = by === 'device' ? NO_DEVICE : by === 'integration' ? NO_INTEGRATION : NO_LABEL;
  const keys = (e: HaEntity): string[] => (by === 'device' ? [e.device_id ?? NONE] : by === 'integration' ? [e.platform ?? NONE] : e.label_ids?.length ? e.label_ids : [NONE]);
  const title = (k: string) => (k === NONE ? none : by === 'device' ? (lk.devices.get(k) ?? k) : by === 'label' ? (lk.labels.get(k) ?? k) : k);
  return [...bucket(list, keys)].map(([k, items]) => subgroup(leaf(by, seg(by, k), title(k), items), by, lk)).sort(byTitle);
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
