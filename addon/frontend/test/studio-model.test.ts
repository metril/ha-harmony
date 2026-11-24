import { describe, expect, it } from 'vitest';
import { Assigner, moveCursor, type Spot } from '../src/lib/assigner';
import type { Config, Device, Page } from '../src/lib/model';
import {
  assign, deviceChips, duplicatePage, errorTarget, firstFree, fits, groupCommand, isRefusal, labelRoom, movePage, moveBox, pageBytes, pageIndexForLayer,
  placeWidget, resizeBox, setKey, setWidget, wireIconOnly, studioChecks, labelMeter, templatePage, trimToBytes, defaultService, type Source, type Target,
} from '../src/lib/studio-model';

const cfg = (): Config => ({
  devices: [
    { id: 'avr', name: 'AV receiver', power: { toggle: { proto: 'rc5', addr: 1, cmd: 1 } }, commands: { vol_up: {}, vol_down: {}, mute: {}, play: {}, '5': {}, menu: {} }, inputs: { hdmi1: {} } },
    { id: 'tv', name: 'TV', commands: { menu: {} } },
  ],
  activities: [
    { id: 'watch', name: 'Watch TV', keys: { select: { device: 'tv', command: 'menu' } } },
    { id: 'stream', name: 'Stream' },
  ],
  keys: { menu: { device: 'tv', command: 'menu' } },
  pages: [
    { id: 'home', kind: 'home', cols: 4, rows: 6, title: 'Home', widgets: [{ type: 'button', id: 'a', x: 0, y: 0, w: 2, h: 1, label: 'A', action: { none: true } }] },
    { id: 'p2', cols: 4, rows: 2, activity: 'stream', widgets: [] } as Page,
  ],
});
const vol: Source = { kind: 'command', device: 'avr', name: 'vol_up' };

describe('command grouping heuristic', () => {
  it.each([
    ['power_on', 'power'], ['power_off', 'power'], ['standby', 'power'], ['vol_up', 'volume'], ['mute', 'volume'], ['volume_down', 'volume'],
    ['up', 'navigation'], ['ok', 'navigation'], ['menu', 'navigation'], ['channel_up', 'navigation'], ['prev_channel', 'navigation'],
    ['play', 'transport'], ['pause', 'transport'], ['fast_forward', 'transport'], ['skip_next', 'transport'], ['rew', 'transport'],
    ['5', 'numbers'], ['num_7', 'numbers'], ['digit0', 'numbers'], ['input_hdmi1', 'inputs'], ['hdmi2', 'inputs'], ['source', 'inputs'], ['subtitle', 'other'], ['aspect', 'other'],
    ['source_toggle', 'other'], ['input_toggle', 'other'], ['toggle_source', 'other'], ['input_next', 'other'], ['source_cycle', 'other'], ['power_toggle', 'power'], ['vol_up', 'volume'], ['volume_mute', 'volume'], ['mute', 'volume'], ['ch_up', 'navigation'], ['ch_down', 'navigation'], ['prev_channel', 'navigation'],
  ])('%s -> %s', (name, group) => expect(groupCommand(name)).toBe(group));
  it('only declared inputs land under Inputs', () => {
    const g = deviceChips({ id: 'x', commands: { source_toggle: { proto: 'nec', addr: 1, cmd: 1 }, subtitle: { proto: 'nec', addr: 1, cmd: 2 } }, inputs: { tv: { proto: 'nec', addr: 1, cmd: 3 } } } as Device);
    expect(g.other.map((c) => c.name)).toEqual(['source_toggle', 'subtitle']);
    expect(g.inputs.map((c) => c.name)).toEqual(['tv']);
  });
  it('builds grouped chips with built-in power and inputs', () => {
    const g = deviceChips(cfg().devices![0]!);
    expect(g.power.map((c) => c.name)).toEqual(['power_on', 'power_off']);
    expect(g.volume.map((c) => c.name)).toEqual(['vol_up', 'vol_down', 'mute']);
    expect(g.inputs[0]).toMatchObject({ kind: 'input', name: 'hdmi1' });
    expect(g.numbers).toHaveLength(1);
  });
});

describe('hard key edits', () => {
  it('writes the layer, inherits with default, and does nothing with none', () => {
    let c = setKey(cfg(), 'watch', 'volume_up', { device: 'avr', command: 'vol_up' });
    expect(c.activities![0]!.keys).toMatchObject({ volume_up: { device: 'avr', command: 'vol_up' }, select: {} });
    c = setKey(c, 'watch', 'volume_up', 'none');
    expect(c.activities![0]!.keys!['volume_up']).toEqual({ none: true });
    c = setKey(c, 'watch', 'volume_up', 'default');
    expect(c.activities![0]!.keys!['volume_up']).toBeUndefined();
    c = setKey(setKey(c, 'watch', 'select', 'default'), 'stream', 'x', 'default');
    expect(c.activities![0]!.keys).toBeUndefined();
    expect(setKey(cfg(), 'default', 'menu', 'default').keys).toEqual({});
  });
  it('does not mutate its input', () => {
    const c = cfg();
    setKey(c, 'default', 'up', { activity: 'off' });
    expect(c.keys).toEqual({ menu: { device: 'tv', command: 'menu' } });
  });
});

describe('assign (drop rules)', () => {
  const ok = (r: ReturnType<typeof assign>) => {
    if (isRefusal(r)) throw new Error(r.refused);
    return r;
  };
  it('a command on a hard key writes the current layer', () => {
    const r = ok(assign(cfg(), 'watch', vol, { kind: 'key', name: 'volume_up' }));
    expect(r.config.activities![0]!.keys!['volume_up']).toEqual({ device: 'avr', command: 'vol_up' });
    expect(r.message).toContain('overriding Default');
    expect(ok(assign(cfg(), 'default', vol, { kind: 'key', name: 'volume_up' })).config.keys!['volume_up']).toBeTruthy();
  });
  it('a command on the screen makes a button at that cell, or the first free one', () => {
    const r = ok(assign(cfg(), 'default', vol, { kind: 'cell', page: 0, x: 2, y: 3 }));
    const w = r.config.pages![0]!.widgets[1]!;
    expect(w).toMatchObject({ type: 'button', x: 2, y: 3, label: 'Vol up', action: { device: 'avr', command: 'vol_up' } });
    expect(r.widget).toEqual({ page: 0, index: 1 });
    const occupied = ok(assign(cfg(), 'default', vol, { kind: 'cell', page: 0, x: 0, y: 0 }));
    expect(occupied.config.pages![0]!.widgets[1]).toMatchObject({ x: 2, y: 0 }); // first free after the occupied cell
  });
  it('shrinks a new widget so it fits and refuses a full page', () => {
    const c = cfg();
    c.pages![1]!.widgets = [{ type: 'label', x: 1, y: 0, w: 1 }, { type: 'label', x: 3, y: 0, w: 1 }, { type: 'label', x: 0, y: 1, w: 4 }];
    const r = ok(assign(c, 'default', vol, { kind: 'cell', page: 1, x: 0, y: 0 }));
    expect(r.config.pages![1]!.widgets[3]).toMatchObject({ x: 0, y: 0, w: 1 });
    c.pages![1]!.widgets.push({ type: 'label', x: 0, y: 0, w: 1 }, { type: 'label', x: 2, y: 0, w: 1 });
    expect(isRefusal(assign(c, 'default', vol, { kind: 'cell', page: 1, x: 0, y: 0 }))).toBe(true);
  });
  it('an activity on a page makes its start button; all off too', () => {
    const r = ok(assign(cfg(), 'default', { kind: 'activity', id: 'stream' }, { kind: 'cell', page: 0, x: 0, y: 2 }));
    expect(r.config.pages![0]!.widgets[1]).toMatchObject({ label: 'Stream', action: { activity: 'stream' } });
    expect(ok(assign(cfg(), 'default', { kind: 'alloff' }, { kind: 'key', name: 'home' })).config.keys!['home']).toEqual({ activity: 'off' });
  });
  it('a light makes a tile, a scene a button with a Home Assistant action', () => {
    const t = ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'light.lamp', name: 'Lamp' }, { kind: 'cell', page: 0, x: 0, y: 1 }));
    expect(t.config.pages![0]!.widgets[1]).toMatchObject({ type: 'tile', entity: 'light.lamp', label: 'Lamp' });
    const s = ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'scene.movie' }, { kind: 'cell', page: 0, x: 0, y: 1 }));
    expect(s.config.pages![0]!.widgets[1]).toMatchObject({ type: 'button', action: { ha: { service: 'scene.turn_on', target: { entity_id: 'scene.movie' } } } });
    expect(defaultService('light.x')).toBe('light.toggle');
  });
  it('refuses Home Assistant actions without backend support, and widgets on keys', () => {
    expect(isRefusal(assign(cfg(), 'default', { kind: 'entity', entity_id: 'scene.movie' }, { kind: 'key', name: 'red' }, false))).toBe(true);
    expect(isRefusal(assign(cfg(), 'default', { kind: 'widget', type: 'button' }, { kind: 'key', name: 'red' }))).toBe(true);
    // a state tile needs no ha action
    expect(isRefusal(assign(cfg(), 'default', { kind: 'entity', entity_id: 'light.a' }, { kind: 'cell', page: 0, x: 0, y: 1 }, false))).toBe(false);
  });
  it('dropping on an existing button replaces its action; on a tile only entities work', () => {
    const r = ok(assign(cfg(), 'default', vol, { kind: 'widget', page: 0, index: 0 }));
    expect(r.config.pages![0]!.widgets[0]!.action).toEqual({ device: 'avr', command: 'vol_up' });
    const c = cfg();
    c.pages![0]!.widgets.push({ type: 'tile', id: 't', x: 0, y: 1, entity: 'sensor.a' });
    // an action on a tile sets its tap when the add-on can store it, and is refused with a reason otherwise
    expect(isRefusal(assign(c, 'default', vol, { kind: 'widget', page: 0, index: 1 }, false))).toBe(true);
    expect(ok(assign(c, 'default', vol, { kind: 'widget', page: 0, index: 1 })).config.pages![0]!.widgets[1]!['tap']).toEqual({ device: 'avr', command: 'vol_up' });
    expect(ok(assign(c, 'default', { kind: 'none' }, { kind: 'widget', page: 0, index: 1 })).config.pages![0]!.widgets[1]!['tap']).toBe('none');
    expect(ok(assign(c, 'default', { kind: 'entity', entity_id: 'sensor.b' }, { kind: 'widget', page: 0, index: 1 })).config.pages![0]!.widgets[1]!['entity']).toBe('sensor.b');
  });
  it('a light or switch becomes a tile whose automatic tap toggles it; a scene or script becomes a button', () => {
    const light = ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'light.lamp', name: 'Lamp' }, { kind: 'cell', page: 0, x: 0, y: 4 }));
    const tile = light.config.pages![0]!.widgets[1]!;
    expect(tile).toMatchObject({ type: 'tile', entity: 'light.lamp' });
    expect('tap' in tile).toBe(false); // automatic: light.toggle
    expect(light.message).toContain('light.toggle');
    expect(ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'switch.plug' }, { kind: 'cell', page: 0, x: 0, y: 4 })).message).toContain('switch.toggle');
    const scene = ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'scene.movie', name: 'Movie' }, { kind: 'cell', page: 0, x: 0, y: 4 }));
    expect(scene.config.pages![0]!.widgets[1]).toMatchObject({ type: 'button', action: { ha: { service: 'scene.turn_on', target: { entity_id: 'scene.movie' } } } });
    expect(ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'script.goodnight' }, { kind: 'cell', page: 0, x: 0, y: 4 })).config.pages![0]!.widgets[1]).toMatchObject({ type: 'button', action: { ha: { service: 'script.turn_on' } } });
    expect(ok(assign(cfg(), 'default', { kind: 'entity', entity_id: 'sensor.t' }, { kind: 'cell', page: 0, x: 0, y: 4 })).message).not.toContain('a tap runs');
  });
  it('widget types make placeholder widgets', () => {
    const r = ok(assign(cfg(), 'default', { kind: 'widget', type: 'button' }, { kind: 'cell', page: 0, x: 0, y: 4 }));
    expect(r.config.pages![0]!.widgets[1]).toMatchObject({ type: 'button', action: { none: true } });
  });
});

describe('grid maths', () => {
  const p = cfg().pages![0]!;
  it('refuses overlap and out of grid', () => {
    expect(fits(p, { x: 1, y: 0, w: 1, h: 1 })).toBe(false);
    expect(fits(p, { x: 2, y: 0, w: 2, h: 1 })).toBe(true);
    expect(fits(p, { x: 3, y: 0, w: 2, h: 1 })).toBe(false);
    expect(fits(p, { x: 0, y: 5, w: 1, h: 2 })).toBe(false);
    expect(fits(p, { x: 0, y: 0, w: 2, h: 1 }, 0)).toBe(true); // itself is ignored
  });
  it('moves and resizes by steps, null when blocked', () => {
    expect(moveBox(p, 0, 1, 0)).toEqual({ x: 1, y: 0, w: 2, h: 1 });
    expect(moveBox(p, 0, -1, 0)).toBeNull();
    expect(resizeBox(p, 0, 2, 0)).toEqual({ x: 0, y: 0, w: 4, h: 1 });
    expect(resizeBox(p, 0, 3, 0)).toBeNull();
    expect(resizeBox(p, 0, -2, 0)).toBeNull();
    const c = placeWidget(cfg(), 0, 0, { x: 1, y: 2, w: 1, h: 1 });
    expect(c.pages![0]!.widgets[0]).toMatchObject({ x: 1, y: 2, w: 1, h: 1 });
  });
  it('finds the first free cell, wrapping from a start', () => {
    expect(firstFree(p)).toEqual({ x: 2, y: 0 });
    expect(firstFree(p, 4, 1)).toEqual({ x: 0, y: 1 });
    expect(firstFree(p, 1, 1, { x: 3, y: 5 })).toEqual({ x: 3, y: 5 });
  });
});

describe('byte counter', () => {
  it('follows the v2 rule: header 11 + title, widget 5 + optional + label', () => {
    const base: Page = { id: 'x', cols: 4, rows: 6, widgets: [] };
    expect(pageBytes(base)).toBe(11);
    expect(pageBytes({ ...base, title: 'é' })).toBe(13);
    const w = { type: 'button', x: 0, y: 0, label: 'Go', action: { none: true } };
    expect(pageBytes({ ...base, widgets: [w] })).toBe(11 + 5 + 2);
    expect(pageBytes({ ...base, widgets: [{ ...w, role: 'primary', icon: 'tv' }] })).toBe(11 + 5 + 1 + 1 + 2);
    expect(pageBytes({ ...base, widgets: [{ ...w, nav: 'next', action: undefined }] })).toBe(11 + 5 + 1 + 1 + 2);
  });
  it('align rides in the kind byte and costs nothing', () => {
    const base: Page = { id: 'x', cols: 4, rows: 6, widgets: [] };
    const w = { type: 'button', x: 0, y: 0, label: 'Go', action: { none: true } };
    expect(pageBytes({ ...base, widgets: [{ ...w, align: 'right' }] })).toBe(11 + 5 + 2);
    expect(pageBytes({ ...base, widgets: [{ ...w, align: 'left' }] })).toBe(11 + 5 + 2);
  });
  it('limits a label to 24 bytes and to what is left of the page budget', () => {
    const base: Page = { id: 'x', cols: 4, rows: 6, widgets: [{ type: 'button', x: 0, y: 0, label: 'abc' }] };
    expect(labelRoom(base, 0)).toBe(24);
    expect(labelRoom(base, 0, 11 + 5 + 10)).toBe(10);
    expect(trimToBytes('ééééé', 5)).toBe('éé');
  });
});

describe('pages', () => {
  it('picks the page of a layer: the activity page, else Home', () => {
    expect(pageIndexForLayer(cfg(), 'stream')).toBe(1);
    expect(pageIndexForLayer(cfg(), 'watch')).toBe(0);
    expect(pageIndexForLayer(cfg(), 'default')).toBe(0);
  });
  it('reorders, duplicates with fresh ids, and builds templates from the config', () => {
    expect(movePage(cfg(), 0, 1).pages!.map((x) => x.id)).toEqual(['p2', 'home']);
    const d = duplicatePage(cfg(), 0);
    expect(d.pages!.map((x) => x.id)).toEqual(['home', 'home_copy', 'p2']);
    expect(d.pages![1]!.kind).toBeUndefined();
    expect(d.pages![1]!.widgets[0]!.id).not.toBe('a');
    const t = templatePage(cfg(), 'activities');
    expect(t.widgets.map((w) => w.label)).toEqual(['Watch TV', 'Stream', 'All Off']);
    expect(templatePage(cfg(), 'lights', { entities: [{ entity_id: 'light.a', name: 'A' }, { entity_id: 'switch.b' }] }).widgets).toHaveLength(1);
    expect(templatePage(cfg(), 'numbers').widgets).toHaveLength(1);
  });
  it('maps validation paths to canvas targets', () => {
    expect(errorTarget(['pages', 0, 'widgets', 2, 'label'])).toEqual({ kind: 'widget', page: 0, index: 2, field: 'label' });
    expect(errorTarget(['pages', 1])).toEqual({ kind: 'page', page: 1 });
    expect(errorTarget(['keys', 'menu'])).toMatchObject({ kind: 'key', key: 'menu' });
    expect(errorTarget(['devices', 0])).toBeNull();
  });
});

describe('assignment state machine', () => {
  const key: Target = { kind: 'key', name: 'volume_up' };
  const mk = () => {
    const log: [Source, Target][] = [];
    return { a: new Assigner((s, t) => log.push([s, t])), log };
  };
  it('click then click commits; clicking the source again cancels', () => {
    const { a, log } = mk();
    a.pick(vol);
    expect(a.phase.name).toBe('picked');
    a.pick(vol);
    expect(a.phase.name).toBe('idle');
    a.pick(vol);
    expect(a.choose(key)).toBe(true);
    expect(log).toEqual([[vol, key]]);
    expect(a.choose(key)).toBe(false);
  });
  it('drag commits on a valid target, ends on nothing, and Esc cancels', () => {
    const { a, log } = mk();
    a.startDrag(vol, 1, 1);
    a.moveDrag(5, 5, key);
    expect(a.cursor).toEqual(key);
    expect(a.drop(key)).toBe(true);
    a.startDrag(vol, 1, 1);
    expect(a.drop(null)).toBe(false);
    a.startDrag(vol, 1, 1);
    a.cancel();
    expect(a.phase.name).toBe('idle');
    expect(log).toHaveLength(1);
  });
  it('keyboard cursor moves between spots by direction and commits with choose', () => {
    const spots: Spot[] = [
      { id: 'a', target: { kind: 'key', name: 'a' }, x: 0, y: 0 },
      { id: 'b', target: { kind: 'key', name: 'b' }, x: 10, y: 0 },
      { id: 'c', target: { kind: 'key', name: 'c' }, x: 0, y: 10 },
    ];
    expect(moveCursor(spots, null, 'right')?.id).toBe('a');
    expect(moveCursor(spots, 'a', 'right')?.id).toBe('b');
    expect(moveCursor(spots, 'a', 'down')?.id).toBe('c');
    expect(moveCursor(spots, 'a', 'left')?.id).toBe('a');
    const { a, log } = mk();
    a.pick(vol);
    a.choose(moveCursor(spots, 'a', 'down')!.target);
    expect(log[0]![1]).toEqual({ kind: 'key', name: 'c' });
  });
});

describe('icon-only state', () => {
  const pg = { id: 'p', cols: 4, rows: 2, widgets: [{ type: 'button', id: 'a', x: 0, y: 0, w: 1, h: 1, label: 'Apps', icon: 'apps' }] } as Page;
  const pp = (io: boolean) => ({ wire_pages: [{ page: { cols: 4, rows: 2, widgets: [{ icon_only: io }] } }] }) as never;
  it('follows the compiled widget (the compiler defaults it on), else the config', () => {
    expect(wireIconOnly(pg, pp(true), 0)).toBe(true);
    expect(wireIconOnly(pg, pp(false), 0)).toBe(false);
    expect(wireIconOnly(pg, null, 0)).toBe(false);
  });
  it('an explicit false survives setWidget', () => {
    const c = { pages: [pg] } as Config;
    expect(setWidget(c, 0, 0, { icon_only: false }).pages![0]!.widgets[0]!.icon_only).toBe(false);
  });
});

describe('multi-screen fixed pages', () => {
  const labels = (i: number) => (i === 15 ? 'Power Toggle' : i === 16 ? 'Context Menu' : `B${i}`);
  const page = { id: 'p', title: 'Pg', layout: 'fixed', cols: 12, rows: 7, widgets: Array.from({ length: 20 }, (_, i) => ({ type: 'button', id: `b${i}`, x: (i % 3) * 4, y: Math.floor(i / 3), w: 4, h: 1, label: labels(i), icon: 'none', action: { none: true } })) } as unknown as Page;
  const ww = (i: number, label: string) => ({ kind: 'button', x: (i % 3) * 4, y: Math.floor(i / 3), w: 4, h: 1, label, shape: 'card', icon: 0, icon_only: false });
  const screen = (from: number, to: number, rows: number, lab: (i: number) => string) => ({ page: { cols: 12, rows, fixed: true, widgets: Array.from({ length: to - from }, (_, k) => ww(k, lab(from + k))) }, widgets: to - from });
  const pp = { wire_pages: [screen(0, 15, 5, labels), screen(15, 20, 2, (i) => (i === 15 ? 'Power' : i === 16 ? 'Menu' : `B${i}`))] } as never;
  it('judges screen-2 widgets on their own compiled labels', () => {
    const c = { devices: [], activities: [], pages: [page] } as unknown as Config;
    const out = studioChecks(c, () => pp, true).filter((k) => k.kind === 'label' && (k.index === 15 || k.index === 16));
    expect(out).toEqual([]);
    expect(labelMeter(page, pp, 15)?.over).toBe(0);
  });
});
