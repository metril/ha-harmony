import '../src/components/hh-ha-browser';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { HaCatalog, HaEntity } from '../src/lib/api';
import {
  NONE, NO_AREA, NO_LABEL, SUBGROUP_MIN, availableGroupBys, emptyFilters, enrich, facet, filterCount, filterEntities, groupEntities, groupIds, highlight, pushRecent, queryWords, readList, reasonText, toggled, typeOf, writeList,
} from '../src/lib/ha-nav';

const DOMS = ['light', 'switch', 'sensor', 'scene', 'script', 'media_player', 'climate', 'cover', 'fan', 'input_boolean'];
/** n entities over 25 areas on 4 floors, 60 integrations, 12 labels (the add-on's wire shape) */
export function catalog(n = 3000): HaCatalog {
  const entities: HaEntity[] = [];
  for (let i = 0; i < n; i++) {
    const dom = DOMS[i % DOMS.length] as string;
    entities.push({
      entity_id: `${dom}.thing_${i}`,
      name: `Thing ${i} ${dom}`,
      domain: dom,
      ...(i % 40 === 39 ? {} : { area_id: `a${i % 25}` }),
      device_id: `d${i % 700}`,
      platform: `integ${i % 60}`,
      ...(i % 5 === 0 ? { label_ids: [`l${i % 12}`, 'shared'] } : {}),
    });
  }
  return {
    entities,
    areas: Array.from({ length: 25 }, (_, i) => ({ id: `a${i}`, name: `Area ${i}`, floor_id: `f${Math.floor(i / 7)}`, floor: `Floor ${Math.floor(i / 7)}` })),
    devices: Array.from({ length: 700 }, (_, i) => ({ id: `d${i}`, name: `Device ${i}` })),
    labels: [...Array.from({ length: 12 }, (_, i) => ({ id: `l${i}`, name: `Label ${i}` })), { id: 'shared', name: 'Shared' }],
    registries: true,
  };
}
const small = enrich({
  entities: [
    { entity_id: 'light.lamp', name: 'Living room lamp', domain: 'light', area_id: 'lr', device_id: 'd1', platform: 'hue', label_ids: ['cozy'] },
    { entity_id: 'switch.plug', name: 'Plug', domain: 'switch', area_id: 'kt', device_id: 'd2', platform: 'tplink' },
    { entity_id: 'scene.movie', name: 'Movie', domain: 'scene' },
    { entity_id: 'sensor.temp', name: 'Temperature', domain: 'sensor', area_id: 'kt', platform: 'hue' },
  ],
  areas: [{ id: 'lr', name: 'Living room', floor_id: 'f1', floor: 'Ground' }, { id: 'kt', name: 'Kitchen', floor_id: 'f1', floor: 'Ground' }],
  devices: [{ id: 'd1', name: 'Hue bulb' }, { id: 'd2', name: 'Smart plug' }],
  labels: [{ id: 'cozy', name: 'Cozy' }],
});
const S = small.entities;
const lk = small.lookups;
const f = (patch: Partial<ReturnType<typeof emptyFilters>>) => ({ ...emptyFilters(), ...patch });
const flat = enrich({ ...{ entities: S.map((e) => ({ ...e })) }, areas: [{ id: 'lr', name: 'Living room' }, { id: 'kt', name: 'Kitchen' }], devices: [{ id: 'd1', name: 'Hue bulb' }, { id: 'd2', name: 'Smart plug' }], labels: [{ id: 'cozy', name: 'Cozy' }] });

describe('search and filters', () => {
  it('multi-word AND over name, entity id, area, device, integration and label names', () => {
    const ids = (q: string) => filterEntities(S, f({ q })).map((e) => e.entity_id);
    expect(ids('living lamp')).toEqual(['light.lamp']);
    expect(ids('KITCHEN')).toEqual(['switch.plug', 'sensor.temp']);
    expect(ids('kitchen hue')).toEqual(['sensor.temp']); // area AND integration
    expect(ids('cozy')).toEqual(['light.lamp']); // label
    expect(ids('tplink')).toEqual(['switch.plug']); // integration
    expect(ids('smart plug')).toEqual(['switch.plug']); // device
    expect(ids('switch.pl')).toEqual(['switch.plug']); // entity id
    expect(ids('nothing here')).toEqual([]);
  });
  it('filters compose and are keyed by id; facets ignore their own dimension', () => {
    const flt = f({ types: new Set(['sensor']), areas: new Set(['kt']) });
    expect(filterEntities(S, flt).map((e) => e.entity_id)).toEqual(['sensor.temp']);
    expect(filterCount(flt)).toBe(2);
    expect(Object.fromEntries(facet(S, flt, 'types'))).toEqual({ switch: 1, sensor: 1 });
    expect(filterEntities(S, f({ areas: new Set([NONE]) })).map((e) => e.entity_id)).toEqual(['scene.movie']);
    expect(filterEntities(S, f({ areas: new Set(['Kitchen']) }))).toEqual([]); // a name is not a key
    expect(filterEntities(S, f({ labels: new Set([NONE]) })).length).toBe(3);
    expect(typeOf({ entity_id: 'binary_sensor.x' })).toBe('sensor');
    expect(typeOf({ entity_id: 'input_button.x' })).toBe('input');
  });
  it('queryWords and highlight', () => {
    expect(queryWords('  Living  ROOM ')).toEqual(['living', 'room']);
    expect(highlight('Living room lamp', ['room', 'lam'])).toEqual([
      { t: 'Living ', m: false },
      { t: 'room', m: true },
      { t: ' ', m: false },
      { t: 'lam', m: true },
      { t: 'p', m: false },
    ]);
    expect(highlight('abc', [])).toEqual([{ t: 'abc', m: false }]);
  });
});

describe('grouping', () => {
  it('area: floors become the first level; entities without an area go last', () => {
    const gs = groupEntities(S, 'area', lk);
    expect(gs.map((g) => [g.title, g.kind, g.count])).toEqual([['Ground', 'floor', 3], [NO_AREA, 'area', 1]]);
    expect(gs[0]?.children?.map((g) => g.title)).toEqual(['Kitchen', 'Living room']);
    expect(gs[0]?.children?.[1]?.id).toBe('floor:f1/area:lr');
  });
  it('same-named areas on different floors stay two groups; ids with a slash cannot collide', () => {
    const e = enrich({
      entities: [{ entity_id: 'light.a', area_id: 'x1' }, { entity_id: 'light.b', area_id: 'x2' }, { entity_id: 'light.c', area_id: 'a/b' }, { entity_id: 'light.d', area_id: 'a' }],
      areas: [{ id: 'x1', name: 'Hall', floor_id: 'up', floor: 'Up' }, { id: 'x2', name: 'Hall', floor_id: 'down', floor: 'Down' }, { id: 'a/b', name: 'P' }, { id: 'a', name: 'Q' }],
    });
    const gs = groupEntities(e.entities, 'area', e.lookups);
    const ids = groupIds(gs);
    expect(new Set(ids).size).toBe(ids.length);
    expect(gs.filter((g) => g.kind === 'floor').map((g) => g.title).sort()).toEqual(['Down', 'No floor', 'Up']);
    expect(e.lookups.areas.get('x1')?.label).toBe('Hall · Up'); // the dropdown tells them apart
    expect(ids.filter((i) => i.includes('area:a')).length).toBe(2);
  });
  it('area without floors is flat; type, device, integration and label group too', () => {
    expect(groupEntities(flat.entities, 'area', flat.lookups).map((g) => g.title)).toEqual(['Kitchen', 'Living room', NO_AREA]);
    expect(groupEntities(S, 'type').map((g) => g.title)).toEqual(['Lights', 'Switches', 'Scenes', 'Sensors']);
    expect(groupEntities(S, 'device', lk).map((g) => g.title)).toEqual(['Hue bulb', 'Smart plug', 'No device']);
    expect(groupEntities(S, 'integration').map((g) => g.title)).toEqual(['hue', 'tplink', 'No integration']);
  });
  it('an entity with several labels appears under each; "No label" is last', () => {
    const list: HaEntity[] = [{ entity_id: 'light.a', label_ids: ['b', 'a'] }, { entity_id: 'light.b', label_ids: ['b'] }, { entity_id: 'light.c' }];
    const gs = groupEntities(list, 'label', { areas: new Map(), devices: new Map(), labels: new Map([['a', 'A'], ['b', 'B']]) });
    expect(gs.map((g) => [g.title, g.count])).toEqual([['A', 1], ['B', 2], ['No label', 1]]);
    expect(NO_LABEL).toBe('No label');
  });
  it('a group over the threshold is split once more: area by type, type by area', () => {
    const big: HaEntity[] = Array.from({ length: SUBGROUP_MIN + 3 }, (_, i) => ({ entity_id: `${i % 2 ? 'light' : 'switch'}.x${i}`, area_id: 'den' }));
    const lks = enrich({ entities: big, areas: [{ id: 'den', name: 'Den' }] });
    const [den] = groupEntities(big, 'area', lks.lookups);
    expect(den?.items).toBeUndefined();
    expect(den?.children?.map((c) => c.title)).toEqual(['Lights', 'Switches']);
    expect(den?.count).toBe(SUBGROUP_MIN + 3);
    const spread = enrich({ entities: big.map((e, i) => ({ ...e, entity_id: e.entity_id.replace('switch.', 'light.'), area_id: i % 3 ? 'a' : 'b' })), areas: [{ id: 'a', name: 'A' }, { id: 'b', name: 'B' }] });
    const [all] = groupEntities(spread.entities, 'type', spread.lookups);
    expect(all?.children?.map((c) => c.title)).toEqual(['A', 'B']);
    expect(groupEntities(S, 'area', lk)[0]?.children?.[0]?.items?.length).toBe(2); // a small group stays flat
  });
  it('handles a 3,000-entity catalog quickly and the group ids are unique', () => {
    const { entities, lookups } = enrich(catalog());
    const t = performance.now();
    for (const by of ['area', 'type', 'device', 'integration', 'label'] as const) {
      const ids = groupIds(groupEntities(entities, by, lookups));
      expect(new Set(ids).size).toBe(ids.length);
    }
    expect(performance.now() - t).toBeLessThan(2000);
    const gs = groupEntities(entities, 'area', lookups);
    expect(gs.map((g) => g.kind)).toContain('floor');
    expect(gs.reduce((n, g) => n + g.count, 0)).toBe(3000);
  });
  it('hides group-by options without data', () => {
    expect(availableGroupBys(S, true)).toEqual(['area', 'type', 'device', 'integration', 'label']);
    expect(availableGroupBys(S, false)).toEqual(['type']);
    expect(availableGroupBys(S.filter((e) => !e.label_ids), true)).toEqual(['area', 'type', 'device', 'integration']);
  });
});

describe('reasons and remembered state', () => {
  it('explains the 503 reason in plain words', () => {
    expect(reasonText('status_503')).toContain('HTTP 503');
    expect(reasonText('status_401')).toContain('refused');
    expect(reasonText('TimeoutError')).toContain('too long');
    expect(reasonText('ClientConnectorError')).toContain('could not connect');
    expect(reasonText('no_access')).toContain('Supervisor');
    expect(reasonText('')).toContain('could not reach');
  });
  beforeEach(() => localStorage.clear());
  it('lists survive a round trip and a broken or absent localStorage', () => {
    writeList('x', ['a', 'b']);
    expect(readList('x')).toEqual(['a', 'b']);
    localStorage.setItem('hh.ha.y', '{nope');
    expect(readList('y')).toEqual([]);
    const spy = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('denied');
    });
    expect(() => writeList('z', ['1'])).not.toThrow();
    spy.mockRestore();
    expect(pushRecent(['a', 'b', 'c'], 'c')).toEqual(['c', 'a', 'b']);
    expect(pushRecent(Array.from({ length: 8 }, (_, i) => String(i)), 'n').length).toBe(8);
    expect(toggled(['a'], 'a')).toEqual([]);
    expect(toggled(['a'], 'b')).toEqual(['a', 'b']);
  });
});

type El = HTMLElement & { updateComplete: Promise<unknown>; shadowRoot: ShadowRoot };
async function mount(n = 3000, extra: Record<string, unknown> = {}) {
  const { entities, lookups } = enrich(catalog(n));
  const el = document.createElement('hh-ha-browser') as El & Record<string, unknown>;
  Object.assign(el, { entities, lookups, registries: true, loaded: true, ...extra });
  document.body.append(el);
  await el.updateComplete;
  return el;
}
const rows = (el: El) => el.shadowRoot.querySelectorAll('.ent').length;
const until = async (el: El, cond: () => boolean) => {
  for (let i = 0; i < 100 && !cond(); i++) {
    await new Promise((r) => setTimeout(r, 20));
    await el.updateComplete;
  }
  expect(cond()).toBe(true);
};
const chipNamed = (el: El, name: string) => [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.fchip')].find((b) => b.textContent?.trim().startsWith(name))!;

describe('hh-ha-browser', () => {
  beforeEach(() => {
    localStorage.clear();
    document.body.replaceChildren();
  });
  it('starts collapsed with floors and areas, expands on click and remembers it', async () => {
    const el = await mount();
    expect(rows(el)).toBe(0);
    const heads = el.shadowRoot.querySelectorAll<HTMLElement>('.gh');
    expect(heads.length).toBeGreaterThan(3);
    expect(heads[0]?.getAttribute('aria-expanded')).toBe('false');
    heads[0]?.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.gh')?.getAttribute('aria-expanded')).toBe('true');
    expect(JSON.parse(localStorage.getItem('hh.ha.open.palette.area') ?? '[]').length).toBe(1);
  });
  it('searching auto-expands matches and clearing restores the collapsed state', async () => {
    const el = await mount();
    const input = el.shadowRoot.querySelector<HTMLInputElement>('input.q')!;
    input.value = 'thing 1234 script';
    input.dispatchEvent(new Event('input'));
    await until(el, () => rows(el) === 1);
    expect(el.shadowRoot.querySelector('mark')).toBeTruthy();
    expect(el.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('1 of 3,000');
    el.shadowRoot.querySelector<HTMLElement>('.clr')!.click();
    await until(el, () => rows(el) === 0);
  });
  it('never draws more rows than the budget, however many groups are open; Show more adds what it says', async () => {
    const el = await mount();
    [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.tools .btn')].find((b) => b.getAttribute('aria-label') === 'Expand All')!.click();
    await el.updateComplete;
    expect(rows(el)).toBeGreaterThan(50);
    expect(rows(el)).toBeLessThanOrEqual(300);
    const more = el.shadowRoot.querySelector<HTMLButtonElement>('.more')!;
    const said = Number(/Show (\d+)/.exec(more.textContent ?? '')?.[1]);
    const before = rows(el);
    more.click();
    await el.updateComplete;
    expect(rows(el) - before).toBe(said);
  });
  it('the Type list filters with counts, shows an n-filters state and clears', async () => {
    const el = await mount();
    chipNamed(el, 'Type').click();
    await el.updateComplete;
    const lights = [...el.shadowRoot.querySelectorAll<HTMLElement>('.opt')].find((o) => o.textContent?.includes('Lights'))!;
    expect(lights.textContent).toContain('300');
    lights.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.pills')?.textContent).toContain('1 filter');
    expect(el.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('300 of 3,000');
    [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.pills .btn')].find((b) => b.textContent === 'Clear')!.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.pills')).toBeNull();
  });
  it('the Area list tells same-named areas apart and filters by id', async () => {
    const e = enrich({ entities: [{ entity_id: 'light.a', area_id: 'x1' }, { entity_id: 'light.b', area_id: 'x2' }], areas: [{ id: 'x1', name: 'Hall', floor: 'Up', floor_id: 'u' }, { id: 'x2', name: 'Hall', floor: 'Down', floor_id: 'd' }] });
    const el = document.createElement('hh-ha-browser') as El & Record<string, unknown>;
    Object.assign(el, { entities: e.entities, lookups: e.lookups, registries: true, loaded: true });
    document.body.append(el);
    await el.updateComplete;
    chipNamed(el, 'Area').click();
    await el.updateComplete;
    const names = [...el.shadowRoot.querySelectorAll('.opt span')].map((s) => s.textContent);
    expect(names).toEqual(['Hall · Down', 'Hall · Up']);
    el.shadowRoot.querySelector<HTMLButtonElement>('.opt')!.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('1 of 2');
  });
  it('says when the list was cut at the add-on cap', async () => {
    const el = await mount(60, { truncated: 25000 });
    expect(el.shadowRoot.querySelector('.note')?.textContent).toContain('first 60 of 25,000');
  });
  it('groups by type without registries and says areas are unavailable', async () => {
    const el = await mount(60, { registries: false });
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.note')?.textContent).toContain('grouped by type only');
    expect([...el.shadowRoot.querySelectorAll('select option')].map((o) => o.textContent)).toEqual(['Type']);
    expect(chipNamed(el, 'Area')).toBeUndefined();
  });
  it('the options menu holds the diagnostic toggle and a refresh', async () => {
    const el = await mount(60);
    el.shadowRoot.querySelector<HTMLElement>('.fchip.menu')!.click();
    await el.updateComplete;
    let diag: unknown = null;
    let refreshed = 0;
    el.addEventListener('hh-ha-diag', (e) => (diag = (e as CustomEvent).detail));
    el.addEventListener('hh-ha-refresh', () => refreshed++);
    const box = el.shadowRoot.querySelector<HTMLInputElement>('.pop input[type="checkbox"]')!;
    box.click();
    expect(diag).toBe(true);
    [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.pop .btn')].find((b) => b.textContent === 'Refresh the List')!.click();
    expect(refreshed).toBe(1);
  });
  it('unreadable: reason in plain words and a Retry event', async () => {
    const el = document.createElement('hh-ha-browser') as El & Record<string, unknown>;
    Object.assign(el, { entities: null, loaded: true, reason: 'status_503' });
    document.body.append(el);
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('[role="alert"]')?.textContent).toContain('HTTP 503');
    let retried = 0;
    el.addEventListener('hh-ha-retry', () => retried++);
    el.shadowRoot.querySelector<HTMLElement>('.btn')!.click();
    expect(retried).toBe(1);
  });
});
