import '../src/components/hh-ha-browser';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { HaEntity } from '../src/lib/api';
import {
  NO_AREA, NO_LABEL, SUBGROUP_MIN, availableGroupBys, emptyFilters, facet, filterCount, filterEntities, groupEntities, groupIds, highlight, pushRecent, queryWords, readList, reasonText, toggled, typeOf, writeList,
} from '../src/lib/ha-nav';

const DOMS = ['light', 'switch', 'sensor', 'scene', 'script', 'media_player', 'climate', 'cover', 'fan', 'input_boolean'];
/** n entities over 25 areas on 4 floors, 60 integrations, 12 labels */
export function catalog(n = 3000): { entities: HaEntity[]; areas: { name: string; floor?: string }[] } {
  const entities: HaEntity[] = [];
  for (let i = 0; i < n; i++) {
    const dom = DOMS[i % DOMS.length] as string;
    const area = i % 40 === 39 ? undefined : `Area ${i % 25}`;
    entities.push({
      entity_id: `${dom}.thing_${i}`,
      name: `Thing ${i} ${dom}`,
      domain: dom,
      ...(area ? { area } : {}),
      device: `Device ${i % 700}`,
      platform: `integ${i % 60}`,
      ...(i % 5 === 0 ? { labels: [`Label ${i % 12}`, 'Shared'] } : {}),
    });
  }
  const areas = Array.from({ length: 25 }, (_, i) => ({ name: `Area ${i}`, floor: `Floor ${Math.floor(i / 7)}` }));
  return { entities, areas };
}
const small: HaEntity[] = [
  { entity_id: 'light.lamp', name: 'Living room lamp', domain: 'light', area: 'Living room', device: 'Hue bulb', platform: 'hue', labels: ['Cozy'] },
  { entity_id: 'switch.plug', name: 'Plug', domain: 'switch', area: 'Kitchen', device: 'Smart plug', platform: 'tplink' },
  { entity_id: 'scene.movie', name: 'Movie', domain: 'scene' },
  { entity_id: 'sensor.temp', name: 'Temperature', domain: 'sensor', area: 'Kitchen', platform: 'hue' },
];
const f = (patch: Partial<ReturnType<typeof emptyFilters>>) => ({ ...emptyFilters(), ...patch });

describe('search and filters', () => {
  it('multi-word AND over name, entity id, area, device, integration and label', () => {
    const ids = (q: string) => filterEntities(small, f({ q })).map((e) => e.entity_id);
    expect(ids('living lamp')).toEqual(['light.lamp']);
    expect(ids('KITCHEN')).toEqual(['switch.plug', 'sensor.temp']);
    expect(ids('kitchen hue')).toEqual(['sensor.temp']); // area AND integration
    expect(ids('cozy')).toEqual(['light.lamp']); // label
    expect(ids('tplink')).toEqual(['switch.plug']); // integration
    expect(ids('smart plug')).toEqual(['switch.plug']); // device
    expect(ids('switch.pl')).toEqual(['switch.plug']); // entity id
    expect(ids('nothing here')).toEqual([]);
  });
  it('filters compose; facets ignore their own dimension', () => {
    const flt = f({ types: new Set(['sensor']), areas: new Set(['Kitchen']) });
    expect(filterEntities(small, flt).map((e) => e.entity_id)).toEqual(['sensor.temp']);
    expect(filterCount(flt)).toBe(2);
    expect(Object.fromEntries(facet(small, flt, 'types'))).toEqual({ switch: 1, sensor: 1 });
    expect(filterEntities(small, f({ areas: new Set([NO_AREA]) })).map((e) => e.entity_id)).toEqual(['scene.movie']);
    expect(filterEntities(small, f({ labels: new Set([NO_LABEL]) })).length).toBe(3);
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
  it('area: floors become the first level when areas have floors; entities without an area go last', () => {
    const gs = groupEntities(small, 'area', { floorOf: new Map([['Living room', 'Ground'], ['Kitchen', 'Ground']]) });
    expect(gs.map((g) => [g.title, g.kind, g.count])).toEqual([['Ground', 'floor', 3], [NO_AREA, 'area', 1]]);
    expect(gs[0]?.children?.map((g) => g.title)).toEqual(['Kitchen', 'Living room']);
    expect(gs[0]?.children?.[1]?.id).toBe('area:Ground/Living room');
  });
  it('area without floors is flat; type, device, integration and label group too', () => {
    expect(groupEntities(small, 'area').map((g) => g.title)).toEqual(['Kitchen', 'Living room', NO_AREA]);
    expect(groupEntities(small, 'type').map((g) => g.title)).toEqual(['Lights', 'Switches', 'Scenes', 'Sensors']);
    expect(groupEntities(small, 'device').map((g) => g.title)).toEqual(['Hue bulb', 'Smart plug', 'No device']);
    expect(groupEntities(small, 'integration').map((g) => g.title)).toEqual(['hue', 'tplink', 'No integration']);
  });
  it('an entity with several labels appears under each; "No label" is last', () => {
    const list: HaEntity[] = [{ entity_id: 'light.a', labels: ['B', 'A'] }, { entity_id: 'light.b', labels: ['B'] }, { entity_id: 'light.c' }];
    const gs = groupEntities(list, 'label');
    expect(gs.map((g) => [g.title, g.count])).toEqual([['A', 1], ['B', 2], [NO_LABEL, 1]]);
  });
  it('a group over the threshold is split once more: area by type, type by area', () => {
    const big: HaEntity[] = Array.from({ length: SUBGROUP_MIN + 3 }, (_, i) => ({ entity_id: `${i % 2 ? 'light' : 'switch'}.x${i}`, area: 'Den' }));
    const [den] = groupEntities(big, 'area');
    expect(den?.items).toBeUndefined();
    expect(den?.children?.map((c) => c.title)).toEqual(['Lights', 'Switches']);
    expect(den?.count).toBe(SUBGROUP_MIN + 3);
    const spread = big.map((e, i) => ({ ...e, area: i % 3 ? 'A' : 'B' }));
    const [lights] = groupEntities(spread.filter((e) => e.entity_id.startsWith('light')), 'type');
    expect(lights?.items?.length).toBe(7); // under the threshold: no sub-headers
    const [all] = groupEntities(spread.map((e) => ({ ...e, entity_id: e.entity_id.replace('switch.', 'light.') })), 'type');
    expect(all?.children?.map((c) => c.title)).toEqual(['A', 'B']);
    // a small group stays flat
    expect(groupEntities(small, 'area')[0]?.items?.length).toBe(2);
  });
  it('handles a 3000-entity catalog quickly and the group ids are unique', () => {
    const { entities, areas } = catalog();
    const floorOf = new Map(areas.map((a) => [a.name, a.floor as string]));
    const t = performance.now();
    for (const by of ['area', 'type', 'device', 'integration', 'label'] as const) {
      const ids = groupIds(groupEntities(entities, by, { floorOf }));
      expect(new Set(ids).size).toBe(ids.length);
    }
    expect(performance.now() - t).toBeLessThan(2000);
    const gs = groupEntities(entities, 'area', { floorOf });
    expect(gs.map((g) => g.kind)).toContain('floor');
    expect(gs.reduce((n, g) => n + g.count, 0)).toBe(3000);
  });
  it('hides group-by options without data', () => {
    expect(availableGroupBys(small, true)).toEqual(['area', 'type', 'device', 'integration', 'label']);
    expect(availableGroupBys(small, false)).toEqual(['type']);
    expect(availableGroupBys(small.filter((e) => !e.labels), true)).toEqual(['area', 'type', 'device', 'integration']);
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
async function mount(n = 3000) {
  const { entities, areas } = catalog(n);
  const el = document.createElement('hh-ha-browser') as El & Record<string, unknown>;
  Object.assign(el, { entities, areas, registries: true, loaded: true });
  document.body.append(el);
  await el.updateComplete;
  return el;
}
const rows = (el: El) => el.shadowRoot.querySelectorAll('.ent').length;
const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));

describe('hh-ha-browser', () => {
  beforeEach(() => {
    localStorage.clear();
    document.body.replaceChildren();
  });
  it('starts collapsed with area groups, expands on click and remembers it', async () => {
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
    await wait(160);
    await el.updateComplete;
    expect(rows(el)).toBe(1);
    expect(el.shadowRoot.querySelector('mark')).toBeTruthy();
    expect(el.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('1 of 3,000');
    el.shadowRoot.querySelector<HTMLElement>('.clr')!.click();
    await el.updateComplete;
    expect(rows(el)).toBe(0);
  });
  it('never draws more rows than the budget, however many groups are open', async () => {
    const el = await mount();
    el.shadowRoot.querySelectorAll<HTMLElement>('.gh');
    [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.tools .btn')].find((b) => b.textContent === 'Expand all')!.click();
    await el.updateComplete;
    expect(rows(el)).toBeGreaterThan(50);
    expect(rows(el)).toBeLessThanOrEqual(300);
    expect(el.shadowRoot.querySelector('.more')).toBeTruthy();
  });
  it('type chips filter with counts, show an n-filters state and clear', async () => {
    const el = await mount();
    const chip = [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.fchip')].find((b) => b.textContent?.startsWith('Lights'))!;
    expect(chip.textContent).toContain('300');
    chip.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.pills')?.textContent).toContain('1 filter');
    expect(el.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('300 of 3,000');
    [...el.shadowRoot.querySelectorAll<HTMLButtonElement>('.pills .btn')].find((b) => b.textContent === 'Clear filters')!.click();
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.pills')).toBeNull();
  });
  it('groups by type without registries and says areas are unavailable', async () => {
    const el = await mount(60);
    Object.assign(el, { registries: false, entities: (el.entities as HaEntity[]).map(({ area, device, platform, labels, ...e }) => (void [area, device, platform, labels], e)) });
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.note')?.textContent).toContain('grouped by type only');
    expect([...el.shadowRoot.querySelectorAll('select option')].map((o) => o.textContent)).toEqual(['Type']);
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
