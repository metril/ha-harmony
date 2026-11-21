import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { IrCandidate, IrDeviceResult } from '../src/lib/api';
import { addActivity, addActivityDevice, addControlPage, addStartButton, addStep, applyHandler, chainOrder, deleteActivity, handlerOf, matchZone, moveChain, moveStep, normName, removeStep, timeline } from '../src/lib/activity-edit';
import { deviceIdFor, duplicateCommand, nameProblem, powerStatus, setPowerMode, sourceLabel, withPorts, withRepeats } from '../src/lib/device-edit';
import { clip, describeAction, haLabel, isHaAction, keyActionLabel } from '../src/lib/studio-model';
import { boundEditor, editorBytes } from '../src/lib/activity-edit';
import { capProblem } from '../src/lib/device-edit';
import { byDomain } from '../src/lib/permissions';
import { proposeFromEntity } from '../src/lib/ha-propose';
import { LIMITS, applyUsageLimits, type Config } from '../src/lib/model';
import { configSizeFigures, meters } from '../src/components/parts';
import { render } from 'lit';
import { USAGE } from './helpers';
import { permissions, tileDefaultTap } from '../src/lib/permissions';
import { changedKeys, changedPages } from '../src/lib/diff';
import { deleteDevice, deleteItem, describeRef, findRefs, renameItem } from '../src/lib/refs';
import { candidateLabel, draftDevice, initial, isOffline, reduce, stepsFor, type WAction, type WState } from '../src/lib/wizard';
import { tvConfig } from './helpers';

const full = (): Config => {
  const c = tvConfig();
  const tv = c.devices?.[0];
  if (tv) tv.commands = { ...tv.commands, vol_up: { proto: 'nec', addr: 4, cmd: 1 }, vol_down: { proto: 'nec', addr: 4, cmd: 2 }, 'KEY_MUTE': { proto: 'nec', addr: 4, cmd: 3 }, ok: { proto: 'nec', addr: 4, cmd: 4 } };
  c.activities = [{ id: 'watch_tv', name: 'Watch TV', devices: [{ device: 'tv', input: 'hdmi1' }, { device: 'hdmi_switch', input: 'port1' }], steps: [{ device: 'tv', command: 'vol_up' }], keys: { select: { device: 'tv', command: 'ok' } } }];
  c.keys = { volume_up: { device: 'tv', command: 'vol_up' } };
  c.pages = [{ id: 'main', kind: 'home', cols: 4, rows: 4, widgets: [{ type: 'button', id: 'b1', x: 0, y: 0, w: 2, label: 'Up', action: { device: 'tv', command: 'vol_up' } }] }];
  return c;
};

describe('reference index', () => {
  it('finds every use of a command', () => {
    const refs = findRefs(full(), { device: 'tv', command: 'vol_up' });
    expect(refs.map((r) => r.kind).sort()).toEqual(['key', 'step', 'widget']);
    expect(refs.map(describeRef).join('|')).toContain('Widget "Up" on page main');
    expect(findRefs(full(), { device: 'tv', command: 'unused' })).toEqual([]);
  });
  it('finds inputs and whole-device uses', () => {
    expect(findRefs(full(), { device: 'tv', input: 'hdmi1' })).toHaveLength(1);
    expect(findRefs(full(), { device: 'tv' }).some((r) => r.kind === 'activity')).toBe(true);
  });
  it('deleting a command clears keys, steps and widgets that used it', () => {
    const n = deleteItem(full(), 'tv', 'command', 'vol_up');
    expect(n.devices?.[0]?.commands?.['vol_up']).toBeUndefined();
    expect(n.keys).toEqual({});
    expect(n.activities?.[0]?.steps).toEqual([]);
    expect(n.pages?.[0]?.widgets[0]?.['action']).toEqual({ none: true });
    expect(findRefs(n, { device: 'tv', command: 'vol_up' })).toEqual([]);
  });
  it('renaming a command keeps every reference and the order', () => {
    const n = renameItem(full(), 'tv', 'command', 'vol_up', 'louder');
    expect(Object.keys(n.devices?.[0]?.commands ?? {}).indexOf('louder')).toBe(Object.keys(full().devices?.[0]?.commands ?? {}).indexOf('vol_up'));
    expect(findRefs(n, { device: 'tv', command: 'louder' })).toHaveLength(3);
  });
  it('renaming and deleting an input updates activities', () => {
    expect(renameItem(full(), 'tv', 'input', 'hdmi1', 'game').activities?.[0]?.devices?.[0]?.input).toBe('game');
    expect(deleteItem(full(), 'tv', 'input', 'hdmi1').activities?.[0]?.devices?.[0]?.input).toBeUndefined();
  });
  it('deleting a device removes it from activities and clears its references', () => {
    const n = deleteDevice(full(), 'tv');
    expect(n.devices?.map((d) => d.id)).toEqual(['hdmi_switch']);
    expect(n.activities?.[0]?.devices?.map((d) => d.device)).toEqual(['hdmi_switch']);
    expect(findRefs(n, { device: 'tv' })).toEqual([]);
  });
});

describe('device edits', () => {
  const d = full().devices?.[0] as NonNullable<Config['devices']>[number];
  it('validates names', () => {
    expect(nameProblem(d, '')).toMatch(/name/);
    expect(nameProblem(d, 'power_on')).toMatch(/reserved/);
    expect(nameProblem(d, 'input_x')).toMatch(/reserved/);
    expect(nameProblem(d, 'x'.repeat(32))).toMatch(/Shorten by 1 byte/);
    expect(nameProblem(d, 'vol_up')).toMatch(/Another/);
    expect(nameProblem(d, 'vol_up', 'vol_up')).toBeNull();
    expect(nameProblem(d, 'é'.repeat(16))).toMatch(/Shorten/); // 32 bytes
  });
  it('duplicates, ports, repeats, power modes', () => {
    const r = duplicateCommand(full(), 'tv', 'vol_up');
    expect(r.name).toBe('vol_up copy');
    expect(r.config.devices?.[0]?.commands?.['vol_up copy']).toEqual(d.commands?.['vol_up']);
    expect(withPorts({ proto: 'nec', addr: 1, cmd: 1 }, 3).port_mask).toBe(3);
    expect(withPorts({ proto: 'nec', addr: 1, cmd: 1, port_mask: 3 }, 7).port_mask).toBeUndefined();
    expect(withRepeats({ proto: 'nec', addr: 1, cmd: 1 }, 20).repeats).toBe(8);
    expect(withRepeats({ proto: 'nec', addr: 1, cmd: 1, repeats: 3 }, 1).repeats).toBeUndefined();
    expect(setPowerMode(full(), 'tv', 'toggle').devices?.[0]?.power).toEqual({ toggle: d.power?.on });
    expect(setPowerMode(full(), 'tv', 'none').devices?.[0]?.power).toBeUndefined();
    expect(powerStatus(d)).toEqual({ text: 'Power On + Off', tone: 'ok' });
    expect(powerStatus({ id: 'x', power: { toggle: { proto: 'nec', addr: 1, cmd: 1 } } }).tone).toBe('ir');
    expect(deviceIdFor(full(), 'Living room TV')).toBe('living_room_tv');
    expect(deviceIdFor(full(), 'TV')).toBe('tv_2');
  });
  it('labels the source', () => {
    expect(sourceLabel(d)).toBe('IR archive · Samsung UE55');
    expect(sourceLabel({ id: 'a' })).toBe('Your Harmony backup');
    expect(sourceLabel({ id: 'a', kind: 'ha' })).toBe('Home Assistant');
  });
});

describe('activity timeline', () => {
  it('generates locked steps with settle gaps, then user steps', () => {
    const t = timeline(full(), 'watch_tv', 'start');
    expect(t.items.map((i) => (i.kind === 'gap' ? `gap${i.ms}` : i.label))).toEqual(['Power On Living room TV', 'gap2000', 'Living room TV → HDMI 1', 'gap2000', 'Power HDMI switch (toggle)', 'HDMI switch → Port 1', 'Living room TV · Volume Up']);
    expect(t.totalMs).toBe(4000);
    expect(t.items.filter((i) => i.kind === 'auto')).toHaveLength(4);
  });
  it('stop runs own steps then powers off in reverse', () => {
    const c = addStep(full(), 'watch_tv', 'stop', { device: 'tv', command: 'ok' }) as Config;
    expect(timeline(c, 'watch_tv', 'stop').items.map((i) => (i.kind === 'gap' ? '' : i.label))).toEqual(['Living room TV · OK', 'Power HDMI switch (toggle)', 'Power Off Living room TV']);
  });
  it('reorders own steps and refuses bad moves', () => {
    let c = full();
    for (const cmd of ['ok', 'vol_down']) c = addStep(c, 'watch_tv', 'start', { device: 'tv', command: cmd }) as Config;
    const order = (x: Config) => x.activities?.[0]?.steps?.map((s) => s['command']);
    expect(order(c)).toEqual(['vol_up', 'ok', 'vol_down']);
    expect(order(moveStep(c, 'watch_tv', 'start', 2, 0))).toEqual(['vol_down', 'vol_up', 'ok']);
    expect(order(moveStep(c, 'watch_tv', 'start', 0, 9))).toEqual(['vol_up', 'ok', 'vol_down']);
    expect(order(removeStep(c, 'watch_tv', 'start', 1))).toEqual(['vol_up', 'vol_down']);
    expect(addStep(c, 'watch_tv', 'start', { activity: 'x' })).toEqual({ refused: 'A step cannot start an activity' });
    let big = c;
    for (let i = 0; i < 20; i++) {
      const r = addStep(big, 'watch_tv', 'start', { device: 'tv', command: 'ok' });
      if (!('refused' in r)) big = r;
    }
    expect(big.activities?.[0]?.steps).toHaveLength(16);
  });
  it('accepts Home Assistant steps', () => {
    const ha = { ha: { service: 'light.turn_off', target: { entity_id: 'light.lamp' } } };
    const c = addStep(full(), 'watch_tv', 'start', ha) as Config;
    expect(timeline(c, 'watch_tv', 'start').items.at(-1)).toMatchObject({ kind: 'user', label: 'light.lamp \u00B7 turn_off' });
  });
  it('chain order is visual only and survives device removal', () => {
    const c = moveChain(full(), 'watch_tv', 0, 1);
    expect(chainOrder(c, 'watch_tv')).toEqual(['hdmi_switch', 'tv']);
    expect(c.activities?.[0]?.devices?.[0]?.device).toBe('tv');
  });
  it('adds devices up to 8 and activities with unique ids', () => {
    expect(addActivityDevice(full(), 'watch_tv', 'tv')).toEqual({ refused: 'That device is already in this activity' });
    const a = addActivity(full(), 'Watch TV');
    expect(a.id).toBe('watch_tv_2');
    expect(addActivity(full(), 'Off').id).toBe('off_activity');
  });
  it('deleting an activity removes what started it', () => {
    let c = full();
    c.keys = { ...c.keys, home: { activity: 'watch_tv' } };
    c.pages?.[0]?.widgets.push({ type: 'button', id: 'b2', label: 'x', action: { activity: 'watch_tv' } });
    c = deleteActivity(c, 'watch_tv');
    expect(c.keys?.['home']).toBeUndefined();
    expect(c.pages?.[0]?.widgets[1]?.['action']).toEqual({ none: true });
  });
});

describe('who handles it: name matching', () => {
  it('normalises spellings', () => {
    expect(normName('KEY_VOLUP')).toBe('volup');
    expect(normName('Vol+')).toBe('volplus');
    expect(normName('ch-')).toBe('chminus');
    expect(normName('Menü ⏎')).toBe('men');
  });
  const dev = (names: string[]) => ({ id: 'd', commands: Object.fromEntries(names.map((n) => [n, { proto: 'nec', addr: 1, cmd: 1 }])) });
  it('matches the table and reports what is missing', () => {
    const r = matchZone(dev(['Vol+', 'volume_down']), 'volume');
    expect(r.matched).toEqual({ volume_up: 'Vol+', volume_down: 'volume_down' });
    expect(r.missing).toEqual(['volume_mute']);
    expect(matchZone(dev(['play_pause', 'stop', 'FF', 'REW']), 'transport')).toEqual({ matched: { play: 'play_pause', pause: 'play_pause', stop: 'stop', rewind: 'REW', fast_forward: 'FF' }, missing: ['record'] });
    expect(matchZone(dev(['ok', 'up', 'down', 'left', 'right', 'back']), 'navigation').missing).toEqual(['menu']);
    expect(matchZone(dev(['ch_up', 'ch_down']), 'channels').matched).toEqual({ channel_up: 'ch_up', channel_down: 'ch_down' });
    expect(matchZone(dev(['banana']), 'volume').matched).toEqual({});
  });
  it('fills the activity layer, remembers it, and never overwrites a hand-set key', () => {
    const c = full();
    c.activities![0]!.keys = { volume_down: { device: 'hdmi_switch', command: 'next' } };
    const r = applyHandler(c, 'watch_tv', 'volume', 'tv');
    expect(r.matched).toEqual({ volume_up: 'vol_up', volume_mute: 'KEY_MUTE' });
    expect(r.kept).toEqual(['volume_down']);
    expect(r.config.activities?.[0]?.keys?.['volume_up']).toEqual({ device: 'tv', command: 'vol_up' });
    expect(handlerOf(r.config, 'watch_tv', 'volume')).toBe('tv');
    // survives a YAML-like round trip of the editor block and is cleaned on change
    const again = applyHandler(r.config, 'watch_tv', 'volume', 'hdmi_switch');
    expect(again.config.activities?.[0]?.keys?.['volume_up']).toBeUndefined();
    expect(again.config.activities?.[0]?.keys?.['volume_down']).toEqual({ device: 'hdmi_switch', command: 'next' });
    expect(again.missing).toEqual(['volume_up', 'volume_down', 'volume_mute']);
    const cleared = applyHandler(r.config, 'watch_tv', 'volume', null);
    expect(handlerOf(cleared.config, 'watch_tv', 'volume')).toBeNull();
    expect(cleared.config.activities?.[0]?.keys?.['volume_mute']).toBeUndefined();
  });
  it('a key the user changed after filling is kept', () => {
    const r = applyHandler(full(), 'watch_tv', 'volume', 'tv').config;
    r.activities![0]!.keys!['volume_up'] = { device: 'tv', command: 'ok' };
    expect(applyHandler(r, 'watch_tv', 'volume', null).config.activities?.[0]?.keys?.['volume_up']).toEqual({ device: 'tv', command: 'ok' });
  });
});

describe('auto-create', () => {
  it('adds a start button on Home and a control page', () => {
    const r = addStartButton(full(), 'watch_tv');
    expect('config' in r && r.config.pages?.[0]?.widgets.at(-1)).toMatchObject({ type: 'button', label: 'Watch TV', action: { activity: 'watch_tv' } });
    const c = addControlPage(full(), 'watch_tv');
    expect(c.pages?.find((p) => p.activity === 'watch_tv')).toMatchObject({ activity: 'watch_tv', back: true, title: 'Watch TV' });
    expect(addStartButton({ ...full(), pages: [] }, 'watch_tv')).toHaveProperty('refused');
  });
});

describe('what changed since the last apply', () => {
  it('lists changed keys per layer and changed pages with their widgets', () => {
    const a = full();
    const b = JSON.parse(JSON.stringify(a)) as Config;
    b.keys = { ...b.keys, menu: { device: 'tv', command: 'ok' } };
    delete (b.keys as Record<string, unknown>)['volume_up'];
    b.activities![0]!.keys = { select: { device: 'tv', command: 'vol_up' } };
    b.pages![0]!.widgets[0]!.label = 'Louder';
    b.pages![0]!.widgets.push({ type: 'label', label: 'New' });
    expect(changedKeys(a, b).map((k) => `${k.layer}:${k.key}:${k.kind}`).sort()).toEqual(['default:menu:added', 'default:volume_up:removed', 'watch_tv:select:changed']);
    expect(changedPages(a, b)).toEqual([{ id: 'main', title: 'main', kind: 'changed', widgets: ['Louder', 'New'] }]);
    expect(changedKeys(a, a)).toEqual([]);
    expect(changedPages(a, { ...a, pages: [] })).toEqual([{ id: 'main', title: 'main', kind: 'removed', widgets: [] }]);
  });
});

describe('permissions and tile defaults', () => {
  it('lists entities and services the draft uses', () => {
    const c = full();
    c.devices?.push({ id: 'shield', kind: 'ha', entity: 'media_player.shield', power: { on: { ha: { service: 'media_player.turn_on', target: { entity_id: 'media_player.shield' } } } } });
    c.pages?.[0]?.widgets.push({ type: 'tile', id: 't2', label: 'Lamp', entity: 'light.lamp', tap: { ha: { service: 'light.turn_on', target: { entity_id: 'light.lamp' } } } });
    c.keys = { ...c.keys, red: { ha: { service: 'scene.turn_on', target: { entity_id: ['scene.movie', 'scene.dim'] } } } };
    const p = permissions(c);
    expect(p.entities.map((e) => e.id)).toEqual(['light.lamp', 'media_player.shield', 'scene.dim', 'scene.movie']);
    expect(p.services.map((s) => s.service)).toEqual(['light.turn_on', 'media_player.turn_on', 'scene.turn_on']);
    expect(permissions({}).entities).toEqual([]);
  });
  it('knows the default tap per domain', () => {
    expect(tileDefaultTap('light.x')).toBe('light.toggle');
    expect(tileDefaultTap('scene.x')).toBe('scene.turn_on');
    expect(tileDefaultTap('input_button.x')).toBe('input_button.press');
    expect(tileDefaultTap('sensor.x')).toBeNull();
  });
});

describe('entity to device proposals', () => {
  it('media_player gets power, volume, transport and says what it cannot know', () => {
    const p = proposeFromEntity({}, { entity_id: 'media_player.shield', name: 'Shield' });
    expect(p?.device).toMatchObject({ kind: 'ha', entity: 'media_player.shield', id: 'shield' });
    expect(p?.device.power?.on?.ha?.service).toBe('media_player.turn_on');
    expect(Object.keys(p?.device.commands ?? {})).toEqual(expect.arrayContaining(['vol_up', 'mute', 'play_pause', 'next']));
    expect(p?.device.commands?.['mute']?.ha?.data).toEqual({ is_volume_muted: true });
    expect(p?.notes.join(' ')).toMatch(/source list/);
  });
  it('remote leaves send_command presets to the user; others have power only; unsupported domains are refused', () => {
    const r = proposeFromEntity({}, { entity_id: 'remote.living' });
    expect(r?.device.commands).toEqual({});
    expect(r?.notes[0]).toMatch(/send_command/);
    expect(proposeFromEntity({}, { entity_id: 'switch.fan_plug' })?.device.power?.off?.ha?.service).toBe('switch.turn_off');
    expect(proposeFromEntity({}, { entity_id: 'sensor.temp' })).toBeNull();
  });
});

// ------------------------------------------------------------------ wizard
const cand = (kind: IrCandidate['kind'], cmd: number, models = ['M1']): IrCandidate => ({ kind, action: { proto: 'nec', addr: 1, cmd }, form: 'proto', count: models.length, models, codesets: 1 });
const result = (): IrDeviceResult => ({ device: { id: 'sony_x', name: 'Sony X', commands: { a: { proto: 'nec', addr: 1, cmd: 1 }, b: { proto: 'nec', addr: 1, cmd: 2 } } }, forms: {}, notes: ['12 gaps shortened'], skipped: [{ name: 'weird', reason: 'unsupported' }], skipped_total: 1, device_type: null, source: {} });
const run = (acts: WAction[], from: WState = initial()) => acts.reduce(reduce, from);
const toTest = (cands: IrCandidate[]) => run([{ t: 'source', source: 'archive' }, { t: 'brand', brand: { name: 'Sony', slug: 'Sony' } }, { t: 'type', type: 'tv' }, { t: 'candidates', candidates: cands }]);

describe('add-device wizard state machine', () => {
  it('walks source -> brand -> type -> test and counts codes', () => {
    const s = toTest([cand('on', 1), cand('off', 2)]);
    expect(s.step).toBe('test');
    expect(candidateLabel(s)).toBe('Code 1 of 2');
    expect(stepsFor('archive').map((x) => x.id)).toEqual(['source', 'brand', 'type', 'test', 'model', 'review']);
  });
  it('"No, next" then "Yes" confirms the second candidate and moves to models', () => {
    const s = run([{ t: 'answer', answer: 'no' }], toTest([cand('on', 1), cand('off', 2, ['A', 'B'])]));
    expect(candidateLabel(s)).toBe('Code 2 of 2');
    const y = reduce(s, { t: 'answer', answer: 'yes' });
    expect(y).toMatchObject({ step: 'model', confirmed: { action: { cmd: 2 } } });
    expect(y.tried.map((t) => t.result)).toEqual(['no', 'yes']);
  });
  it('running out of candidates, or none at all, lands on the model list', () => {
    const s = run([{ t: 'answer', answer: 'skip' }, { t: 'answer', answer: 'no' }], toTest([cand('on', 1), cand('off', 2)]));
    expect(s).toMatchObject({ step: 'model', exhausted: true, confirmed: null });
    expect(toTest([])).toMatchObject({ step: 'model', exhausted: true });
    expect(run([{ t: 'skipTest' }], toTest([cand('on', 1)])).step).toBe('model');
  });
  it('flags truncated candidates', () => {
    const s = run([{ t: 'candidates', candidates: [cand('on', 1)], truncated: true }], run([{ t: 'type', type: 'tv' }], initial()));
    expect(s.truncated).toBe(true);
  });
  it('model -> device -> review ticks every command; toggling and select all work; add builds the draft', () => {
    let s = run([{ t: 'answer', answer: 'yes' }, { t: 'model', model: { file: 'x.json', name: 'X', id: 1 } }], toTest([cand('on', 1)]));
    expect(s.loading).toBe(true);
    s = reduce(s, { t: 'device', result: result() });
    expect(s).toMatchObject({ step: 'review', selected: ['a', 'b'], loading: false });
    s = reduce(s, { t: 'toggle', name: 'a' });
    expect(Object.keys(draftDevice(s)?.commands ?? {})).toEqual(['b']);
    expect(reduce(s, { t: 'all', on: false }).selected).toEqual([]);
    expect(reduce(s, { t: 'all', on: true }).selected).toEqual(['a', 'b']);
  });
  it('archive_offline keeps the step and offers retry; retry clears and loads again', () => {
    let s = reduce(toTest([cand('on', 1)]), { t: 'fail', scope: 'candidates', code: 'archive_offline', msg: 'down' });
    expect(isOffline(s.error)).toBe(true);
    expect(s.step).toBe('test');
    s = reduce(s, { t: 'retry' });
    expect(s).toMatchObject({ error: null, loading: true });
  });
  it('unknown_brand sends you back to pick a brand', () => {
    const s = reduce(toTest([cand('on', 1)]), { t: 'fail', scope: 'candidates', code: 'unknown_brand', msg: 'no such brand' });
    expect(s).toMatchObject({ step: 'brand', brand: null, error: { code: 'unknown_brand' } });
  });
  it('archive_too_large drops the model and stays on the list; other candidate errors still allow model search', () => {
    const base = run([{ t: 'answer', answer: 'yes' }, { t: 'model', model: { file: 'x.json', name: 'X', id: 1 } }], toTest([cand('on', 1)]));
    expect(reduce(base, { t: 'fail', scope: 'device', code: 'archive_too_large', msg: 'big' })).toMatchObject({ step: 'model', model: null, loading: false, error: { code: 'archive_too_large' } });
    expect(reduce(run([{ t: 'type', type: 'tv' }], initial()), { t: 'fail', scope: 'candidates', code: 'archive_error', msg: 'bad' })).toMatchObject({ step: 'model', exhausted: true });
  });
  it('back walks the stepper and drops the result', () => {
    const s = run([{ t: 'answer', answer: 'yes' }, { t: 'model', model: { file: 'x.json', name: 'X', id: 1 } }, { t: 'device', result: result() }, { t: 'back' }], toTest([cand('on', 1)]));
    expect(s).toMatchObject({ step: 'model', result: null });
    expect(run([{ t: 'back' }, { t: 'back' }], s).step).toBe('type');
    expect(reduce(initial(), { t: 'back' }).step).toBe('source');
  });
  it('paste and Home Assistant sources have their own short steppers', () => {
    expect(reduce(initial(), { t: 'source', source: 'paste' }).step).toBe('paste');
    expect(reduce(initial(), { t: 'source', source: 'ha' }).step).toBe('entity');
    expect(reduce(reduce(initial(), { t: 'source', source: 'ha' }), { t: 'draft', result: result() }).step).toBe('review');
  });
});

describe('key map labels', () => {
  const names = new Map([['light.lamp', 'Lamp']]);
  const ha = tvConfig();
  ha.devices?.push({ id: 'shield', name: 'Shield', kind: 'ha', entity: 'media_player.shield', commands: { DPAD_UP: { ha: { service: 'remote.send_command', target: { entity_id: 'remote.shield' } } } } });
  it('Home Assistant actions read "<friendly name> · <service>", with a repeat marker', () => {
    expect(keyActionLabel({ kind: 'ha', service: 'light.toggle', entity: 'light.lamp', label: 'light.toggle light.lamp' }, ha, names)).toBe('Lamp \u00B7 toggle');
    expect(keyActionLabel({ kind: 'ha', service: 'light.toggle', entity: 'light.k', label: 'light.toggle light.k (repeats)' }, ha, names)).toBe('light.k \u00B7 toggle \u21BB');
    expect(keyActionLabel({ kind: 'ha', service: 'scene.turn_on', entity: 'scene.a +1', label: 'x' }, ha, names)).toBe('scene.a +1 \u00B7 turn_on');
    expect(keyActionLabel({ kind: 'device', device: 'shield', command: 'DPAD_UP', label: 'shield: DPAD_UP' }, ha)).toBe('Shield \u00B7 D-Pad Up');
  });
  it('leaves events, IR devices and the rest alone', () => {
    expect(keyActionLabel({ kind: 'ha', event: 'tv', label: 'HA event tv' }, ha)).toBe('HA event tv');
    expect(keyActionLabel({ kind: 'device', device: 'tv', command: 'mute', label: 'tv: mute' }, ha)).toBe('tv: Mute');
  });
  it('describeAction (key popover) uses the same form; long labels clip with an ellipsis', () => {
    expect(describeAction(ha, { ha: { service: 'light.toggle', target: { entity_id: 'light.lamp' }, repeat: true } }, names)).toBe('Lamp \u00B7 toggle \u21BB');
    expect(describeAction(ha, { device: 'shield', command: 'DPAD_UP' })).toBe('Shield \u00B7 D-Pad Up');
    expect(haLabel('light.x', 0, 'light.toggle', false)).toBe('light.x \u00B7 toggle');
    const long = 'Living room ceiling spotlights \u00B7 turn_on';
    expect(clip(long)).toHaveLength(26);
    expect(clip(long).endsWith('\u2026')).toBe(true);
    expect(clip('Lamp \u00B7 toggle')).toBe('Lamp \u00B7 toggle');
  });
  it('knows which actions run through Home Assistant (no IR test-fire)', () => {
    expect(isHaAction(ha, { ha: { service: 'light.toggle', target: { entity_id: 'light.lamp' } } })).toBe(true);
    expect(isHaAction(ha, { device: 'shield', command: 'DPAD_UP' })).toBe(true);
    expect(isHaAction(ha, { device: 'tv', command: 'power_on' })).toBe(false);
    expect(isHaAction(ha, null)).toBe(false);
  });
});

describe('permissions grouped by domain', () => {
  it('lists the services used per entity, domains in order', () => {
    const c: Config = { devices: [], keys: {}, pages: [] };
    for (let i = 0; i < 20; i++) c.keys![`k${i}`] = { ha: { service: i % 2 ? 'light.toggle' : 'switch.turn_on', target: { entity_id: `${i % 2 ? 'light' : 'switch'}.e${i}` } } };
    c.keys!['x'] = { ha: { service: 'light.turn_off', target: { entity_id: 'light.e1' } } };
    const g = byDomain(permissions(c));
    expect(g.map((x) => [x.domain, x.entities.length])).toEqual([['light', 10], ['switch', 10]]);
    expect(g[0]!.entities.find((e) => e.id === 'light.e1')!.services).toEqual(['light.toggle', 'light.turn_off']);
  });
});

describe('hub limits are refused before apply', () => {
  const many = (n: number, kind: 'commands' | 'inputs'): Config => ({ devices: [{ id: 'd', [kind]: Object.fromEntries(Array.from({ length: n }, (_, i) => [`c${i}`, { proto: 'nec', addr: 1, cmd: i % 256 }])) }] });
  beforeEach(() => Object.assign(LIMITS, { devices: 16, commands: 256, inputs: 64 }));
  afterEach(() => Object.assign(LIMITS, { devices: 255, commands: Infinity, inputs: Infinity }));
  it('commands past 256, inputs past 64, devices past 16', () => {
    expect(capProblem(many(255, 'commands'), 'command')).toBeNull();
    expect(capProblem(many(256, 'commands'), 'command')).toContain('at most 256 commands');
    expect(capProblem(many(250, 'commands'), 'command', 7)).toContain('256');
    expect(capProblem(many(64, 'inputs'), 'input')).toContain('at most 64 inputs');
    expect(capProblem({ devices: Array.from({ length: 16 }, (_, i) => ({ id: `d${i}` })) }, 'device')).toContain('at most 16 devices');
  });
  it('Home Assistant devices are not compiled, so they do not count', () => {
    const c = many(256, 'commands');
    c.devices!.push({ id: 'h', kind: 'ha', commands: { a: { ha: { service: 'light.toggle', target: { entity_id: 'light.a' } } } } });
    expect(capProblem(c, 'command', 1, true)).toBeNull();
  });
});

describe('the editor shows the connected hub\'s caps', () => {
  const many = (n: number): Config => ({ devices: [{ id: 'd', commands: Object.fromEntries(Array.from({ length: n }, (_, i) => [`c${i}`, { proto: 'nec', addr: 1, cmd: i % 256 }])) }] });
  afterEach(() => Object.assign(LIMITS, { devices: 255, activities: 16, commands: Infinity, inputs: Infinity }));
  const current = { ...USAGE, devices_max: 255, config_bytes: 3 * 1024 * 1024, config_budget: 8 * 1024 * 1024, config_overhead: 1536 * 1024 };
  it('a current hub has no command or input count limit, so nothing is refused for count', () => {
    applyUsageLimits(current);
    expect(LIMITS.commands).toBe(Infinity);
    expect(capProblem(many(5000), 'command')).toBeNull();
    expect(capProblem(many(5000), 'input', 300)).toBeNull();
    expect(capProblem({ devices: Array.from({ length: 255 }, (_, i) => ({ id: `d${i}` })) }, 'device')).toContain('at most 255 devices');
  });
  it('one "Config size X of Y KiB on this hub" meter replaces the count meters on a current hub', () => {
    const host = document.createElement('div');
    render(meters({ usage: current, config: many(10) } as never), host);
    expect(host.textContent).toContain('Config size');
    expect(host.textContent).toContain('1.5 MiB of 6.5 MiB on this hub');
    expect(host.textContent).not.toContain('IR pool');
    expect(host.textContent).not.toContain('Commands');
    expect(host.textContent).not.toContain('remove commands');
    render(meters({ usage: { ...current, config_bytes: 1536 * 1024 + 800 }, config: many(10) } as never), host);
    expect(host.textContent).toContain('0.8 KiB of 6.5 MiB on this hub');
    render(meters({ usage: { ...current, config_bytes: 1000 }, config: many(10) } as never), host);
    expect(host.textContent).toContain('0 KiB of 6.5 MiB');
    render(meters({ usage: { ...current, config_bytes: 9000 * 1024 }, config: many(10) } as never), host);
    expect(host.textContent).toContain('over, remove commands');
  });
});

describe('config size meter: label and bar come from one function', () => {
  const OH = 1536 * 1024;
  const BUDGET = 8 * 1024 * 1024;
  const cur = { ...USAGE, config_budget: BUDGET, config_overhead: OH };
  function shown(bytes: number) {
    const host = document.createElement('div');
    render(meters({ usage: { ...cur, config_bytes: bytes }, config: {} } as never), host);
    const bar = host.querySelector('.bar i') as HTMLElement;
    return { text: host.querySelector('.meter span')!.textContent!, width: parseFloat(bar.style.width), min: bar.style.minWidth };
  }
  it('an empty config is a sliver, 150 commands about 0.3 %, and the label is the same net figures', () => {
    const empty = shown(OH);
    expect(empty.width).toBe(0);
    const small = shown(OH + 150 * 120);
    const f = configSizeFigures(OH + 150 * 120, BUDGET, OH);
    expect(small.text).toContain(f.text);
    expect(small.width).toBeCloseTo((f.used / f.max) * 100, 3);
    expect(small.width).toBeLessThan(1);
    expect(small.min).toBe('2px');
  });
  it('half and over', () => {
    const half = shown(OH + (BUDGET - OH) / 2);
    expect(half.width).toBeCloseTo(50, 3);
    const over = shown(BUDGET + 1);
    expect(over.width).toBe(100);
    expect(over.text).toContain('remove commands');
  });
});

describe('editor metadata is bounded by construction', () => {
  it('drops entries of deleted activities, foreign chain ids and oversized strings; same object when clean', () => {
    const c: Config = {
      devices: [{ id: 'tv' }],
      activities: [{ id: 'a', devices: [{ device: 'tv' }] }],
      editor: { activities: { a: { chain: ['tv', 'gone'], handlers: { volume: 'x'.repeat(500) } }, dead: { chain: ['tv'] } } },
    };
    const b = boundEditor(c);
    expect(Object.keys(b.editor!['activities'] as object)).toEqual(['a']);
    expect((b.editor!['activities'] as Record<string, { chain: string[]; handlers: Record<string, string> }>)['a']!.chain).toEqual(['tv']);
    expect((b.editor!['activities'] as Record<string, { handlers: Record<string, string> }>)['a']!.handlers['volume']).toHaveLength(100);
    expect(boundEditor(b)).toBe(b);
    expect(boundEditor({ devices: [] })).toEqual({ devices: [] });
  });
  it('stays under the soft cap however many keys the user fills', () => {
    const filled = Object.fromEntries(Array.from({ length: 5000 }, (_, i) => [`k${i}`, { device: 'tv', command: 'x'.repeat(200) }]));
    const c: Config = { devices: [{ id: 'tv' }], activities: [{ id: 'a', devices: [{ device: 'tv' }] }], editor: { activities: { a: { filled } } } };
    const b = boundEditor(c);
    expect(editorBytes(b)).toBeLessThan(32 * 1024);
  });
});
