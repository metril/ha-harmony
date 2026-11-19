import { describe, expect, it } from 'vitest';
import { ApiError, type BtCodes, type BtState } from '../src/lib/api';
import { commandNames, setBtDefaults } from '../src/lib/bt';
import { CmdId, addHostDevice, addRemote, bars, btError, clock, dropRowKey, hidCode, keyLabel, keyOptions, keyProblem, keyRows, nextCountdown, outcome, parseHid, patchRemote, removeRemote, secsLeft, setRowKey } from '../src/lib/bt-model';
import { announce, derive, observe, startRun } from '../src/lib/bt-wizard';
import { deviceChips } from '../src/lib/studio-model';
import { capProblem, irTotals } from '../src/lib/device-edit';
import { counts } from '../src/lib/model';
import { BtPoll } from '../src/lib/bt-poll';
import { vi } from 'vitest';
import { resolveIr } from '../src/lib/studio-model';
import type { Config, Device } from '../src/lib/model';

const codes: BtCodes = {
  builtins: [
    { name: 'Home', page: 12, usage: 0x223, default: true },
    { name: 'DirectionUp', page: 7, usage: 0x52, default: true },
    { name: 'Red', page: 12, usage: 0x69, default: true },
  ],
  remote_keys: [
    { name: 'DirectionUp', page: 7, usage: 0x52, key: 'up' },
    { name: 'Home', page: 12, usage: 0x223, key: 'home' },
  ],
  keys: ['up', 'home', 'menu', 'key_100'],
};
const state = (o: Partial<BtState> = {}): BtState => ({ state: 'ready', discoverable: false, hosts: [], remotes: [], ...o });

describe('error sentences', () => {
  it('never shows a bare code as the main text', () => {
    for (const [status, code] of [[409, 'bt_unknown'], [409, 'hub_offline'], [409, 'bt_unsupported'], [429, 'bt_busy'], [504, 'hub_timeout'], [0, 'network'], [403, 'forbidden']] as const) {
      const t = btError(new ApiError(status, code, 'raw'));
      expect(t).not.toContain(code);
      expect(t.length).toBeGreaterThan(20);
    }
  });
  it('maps the hub refusal code and the add-on validation code', () => {
    expect(btError(new ApiError(502, 'hub_refused', 'x', [], [], { hub_code: 'no_candidate' }))).toContain('scan again');
    expect(btError(new ApiError(422, 'invalid_bt', 'slot out of range', [], [], { bt_code: 'no_le' }))).toContain('no Bluetooth LE');
    expect(btError(new ApiError(502, 'hub_refused', 'odd thing', [], [], { hub_code: 'weird' }))).toContain('odd thing');
    expect(btError(new Error('x'))).toContain('Try again');
  });
});

describe('command ids', () => {
  it('keeps the id across a timeout or lost connection and drops it otherwise', () => {
    const c = new CmdId();
    const a = c.id();
    expect(a).toMatch(/^[a-z0-9]{12}$/);
    c.settle(new ApiError(504, 'hub_timeout', 'x'));
    expect(c.id()).toBe(a);
    c.settle(new ApiError(0, 'network', 'x'));
    expect(c.id()).toBe(a);
    c.settle(new ApiError(502, 'hub_refused', 'x'));
    expect(c.id()).not.toBe(a);
    const b = c.id();
    c.settle();
    expect(c.id()).not.toBe(b);
  });
});

describe('countdown and signal', () => {
  it('projects the hub figure with our clock and adopts a drifted one', () => {
    const s = state({ discoverable: true, discoverable_secs: 100 });
    const c = nextCountdown(null, s, 1000)!;
    expect(secsLeft(c, 31_000)).toBe(70);
    expect(nextCountdown(c, state({ discoverable: true, discoverable_secs: 69 }), 31_000)).toBe(c);
    expect(nextCountdown(c, state({ discoverable: true, discoverable_secs: 40 }), 31_000)?.secs).toBe(40);
    expect(nextCountdown(c, state(), 31_000)).toBeNull();
    expect(secsLeft(c, 999_000)).toBe(0);
    expect(clock(75)).toBe('1:15');
  });
  it('bars from rssi', () => {
    expect([-40, -60, -70, -90, -127].map(bars)).toEqual([4, 3, 2, 1, 0]);
  });
});

describe('pairing outcomes', () => {
  it('explains refused, failed and timeout with a next step', () => {
    for (const r of ['refused', 'failed', 'timeout'] as const) {
      const o = outcome(r, 'device');
      expect(o.title).not.toBe('');
      expect(o.body.length).toBeGreaterThan(30);
    }
    expect(outcome('ok', 'remote', 'Homatics').title).toBe('Paired with Homatics');
  });
});

describe('wizard phases', () => {
  const host = (o: Partial<BtState> = {}) => state({ window: 'host', discoverable: true, discoverable_secs: 100, ...o });
  it('opening, waiting, paired', () => {
    let run = startRun('host', state(), 0);
    expect(derive(run, state(), 1000).t).toBe('opening');
    run = observe(run, host());
    expect(derive(run, host(), 2000).t).toBe('waiting');
    const done = state({ hosts: [{ slot: 0, addr: 'AA:BB:CC:DD:EE:01', name: 'Box', transport: 'le', connected: true }], pairing: { result: 'ok', addr: 'AA:BB:CC:DD:EE:01', name: 'Box', kind: 'host' } });
    expect(derive(run, done, 3000)).toMatchObject({ t: 'paired', addr: 'AA:BB:CC:DD:EE:01', name: 'Box', transport: 'le' });
  });
  it('already paired hosts are not new; a stale result is ignored', () => {
    const old = state({ hosts: [{ slot: 0, addr: 'AA:BB:CC:DD:EE:01', transport: 'le', connected: false }], pairing: { result: 'failed', kind: 'host' } });
    const run = startRun('host', old, 0);
    expect(derive(run, { ...old, ...host() }, 500).t).toBe('waiting');
    expect(derive(run, old, 1000).t).toBe('opening');
  });
  it('refused, failed and a window that closes with nothing', () => {
    let run = observe(startRun('host', state(), 0), host());
    expect(derive(run, state({ pairing: { result: 'refused', kind: 'host' } }), 5000)).toEqual({ t: 'failed', result: 'refused' });
    expect(derive(run, state({ pairing: { result: 'failed' } }), 5000)).toEqual({ t: 'failed', result: 'failed' });
    expect(derive(run, state(), 5000)).toEqual({ t: 'failed', result: 'timeout' });
    run = startRun('host', state(), 0);
    expect(derive(run, state(), 9000)).toEqual({ t: 'failed', result: 'timeout' }); // never saw it open
  });
  it('remote: scanning lists candidates, pick moves to bonding, a new remote is paired', () => {
    let run = startRun('remote', state(), 0);
    const scan = state({ window: 'remote', candidates: [{ addr: 'AA:BB:CC:00:00:01', name: 'Homatics', rssi: -50 }] });
    run = observe(run, scan);
    expect(derive(run, scan, 1000)).toMatchObject({ t: 'scanning', candidates: [{ name: 'Homatics' }] });
    run = { ...run, picked: 'AA:BB:CC:00:00:01' };
    expect(derive(run, scan, 2000)).toEqual({ t: 'bonding', addr: 'AA:BB:CC:00:00:01' });
    const done = state({ remotes: [{ slot: 0, addr: 'AA:BB:CC:00:00:01', name: 'Homatics', connected: true }] });
    expect(derive(run, done, 3000)).toMatchObject({ t: 'paired', addr: 'AA:BB:CC:00:00:01' });
  });
  it('announcements carry no running numbers', () => {
    expect(announce({ t: 'waiting' }, 'host')).not.toMatch(/\d/);
    expect(announce({ t: 'scanning', candidates: [] }, 'remote')).toContain('Looking');
  });
});

describe('HID codes', () => {
  it('parses and formats', () => {
    expect(hidCode(12, 0x223)).toBe('HID:0C:0223');
    expect(parseHid('HID:0C:0223')).toEqual({ page: 12, usage: 0x223 });
    expect(typeof parseHid('HID:09:0001')).toBe('string');
    expect(typeof parseHid('HID:07:0100')).toBe('string');
    expect(typeof parseHid('HID:0C:0400')).toBe('string');
    expect(typeof parseHid('0C:0223')).toBe('string');
  });
});

describe('config edits', () => {
  it('adds a kind: bt device with an upper-case address and a unique id', () => {
    const cfg: Config = { devices: [{ id: 'shield', name: 'Shield', commands: {} } as Device] };
    const r = addHostDevice(cfg, { name: 'Shield', address: 'aa:bb:cc:dd:ee:ff', commands: { Home: 'Home' } });
    expect(r.id).toBe('shield_2');
    expect(r.config.devices?.[1]).toMatchObject({ kind: 'bt', address: 'AA:BB:CC:DD:EE:FF', transport: 'auto', commands: { Home: 'Home' } });
  });
  it('adds, patches and removes bt_remotes entries', () => {
    let cfg = addRemote({}, { name: 'Homatics', address: 'aa:bb:cc:dd:ee:01', keys: { Red: 'red' } });
    expect(cfg['bt_remotes']).toEqual([{ name: 'Homatics', address: 'AA:BB:CC:DD:EE:01', keys: { Red: 'red' } }]);
    cfg = patchRemote(cfg, 0, { keys: {} });
    expect(cfg['bt_remotes']).toEqual([{ name: 'Homatics', address: 'AA:BB:CC:DD:EE:01' }]);
    expect('bt_remotes' in removeRemote(cfg, 0)).toBe(false);
  });
});

describe('key table', () => {
  it('defaults first, your override marked, your own rows after', () => {
    const rows = keyRows(codes, { Home: 'menu', 'HID:0C:00CD': 'play' });
    expect(rows.map((r) => [r.id, r.key, r.mine])).toEqual([['DirectionUp', 'up', false], ['Home', 'menu', true], ['HID:0C:00CD', 'play', true]]);
    expect(rows[1]?.was).toBe('home');
  });
  it('an override equal to the default is cleared; other edits add', () => {
    const row = { id: 'Home', was: 'home' };
    expect(setRowKey({ Home: 'menu' }, row, 'home')).toEqual({});
    expect(setRowKey({}, row, 'menu')).toEqual({ Home: 'menu' });
    expect(setRowKey({}, { id: 'HID:0C:00CD' }, 'play')).toEqual({ 'HID:0C:00CD': 'play' });
    expect(dropRowKey({ a: 'b', c: 'd' }, 'a')).toEqual({ c: 'd' });
  });
  it('a default can be set to does-nothing (null) and reset', () => {
    const row = { id: 'Home', was: 'home' };
    const keys = setRowKey({}, row, null);
    expect(keys).toEqual({ Home: null });
    expect(keyRows(codes, keys)[1]).toMatchObject({ key: null, mine: true });
    expect(setRowKey(keys, row, 'home')).toEqual({});
    expect(setRowKey({ 'HID:0C:00CD': 'play' }, { id: 'HID:0C:00CD' }, null)).toEqual({});
  });
  it('key names get human labels and groups', () => {
    expect([keyLabel('key_100'), keyLabel('volume_up'), keyLabel('power_off_activity'), keyLabel('key_5')]).toEqual(['Digit 0', 'Volume up', 'Power off activity', 'Key 5']);
    const g = keyOptions({ ...codes, keys: ['up', 'play', 'volume_up', 'red', 'key_103', 'favorites', 'zzz'] }).map((x) => [x.group, x.keys.map((k) => k.id)]);
    expect(g).toEqual([['Navigation', ['up', 'favorites']], ['Transport', ['play']], ['Volume and channel', ['volume_up']], ['Colour', ['red']], ['Digits', ['key_103']], ['Other', ['zzz']]]);
  });
  it('validates key names', () => {
    expect(keyProblem(codes, 'menu')).toBeNull();
    expect(keyProblem(codes, 'key_127')).toBeNull();
    expect(keyProblem(codes, 'key_128')).toContain('not a key name');
    expect(keyProblem(codes, '')).toContain('Choose');
  });
});

describe('bt devices in the shared editor code', () => {
  const bt = { id: 'box', kind: 'bt', name: 'Box', address: 'AA:BB:CC:DD:EE:FF' } as Device;
  it('a device without commands has the default set once it is known', () => {
    expect(commandNames(bt)).toEqual([]);
    setBtDefaults(['Home', 'Select']);
    expect(commandNames(bt)).toEqual(['Home', 'Select']);
    expect(Object.values(deviceChips(bt)).flat().map((c) => c.name).sort()).toEqual(['Home', 'Select']);
    expect(commandNames({ id: 'tv', commands: { a: {} } })).toEqual(['a']);
    setBtDefaults([]);
  });
  it('is not IR (no code to test-fire) but its commands count like the backend counts them', () => {
    const cfg: Config = { devices: [{ ...bt, commands: { Home: 'Home' } as never }] };
    expect(resolveIr(cfg, { device: 'box', command: 'Home' })).toBeNull();
    expect(irTotals(cfg)).toEqual({ commands: 1, inputs: 0 });
    setBtDefaults(['Home', 'Select', 'Back']);
    expect(irTotals({ devices: [bt] })).toEqual({ commands: 3, inputs: 0 }); // no `commands`: the default set
    expect(irTotals({ devices: [bt, { id: 'ha', kind: 'ha', commands: { a: {} } } as never] }).commands).toBe(3); // HA is not compiled
    expect(counts({ devices: [bt] }).commands).toBe(3);
    expect(capProblem({ devices: [bt, { id: 'tv', commands: Object.fromEntries(Array.from({ length: 253 }, (_, i) => [`c${i}`, {}])) } as never] }, 'command')).toContain('at most 256');
    setBtDefaults([]);
  });
});

describe('stale pairing results', () => {
  const addrX = 'AA:BB:CC:DD:EE:01';
  const stale = { result: 'ok' as const, addr: addrX, name: 'X', kind: 'host' as const };
  const host = (o: Partial<BtState> = {}) => state({ window: 'host', discoverable: true, discoverable_secs: 100, ...o });
  it('a run for Y that times out after X paired earlier is a timeout, not "Paired with X"', () => {
    const before = state({ gen: 5, hosts: [{ slot: 0, addr: addrX, transport: 'le', connected: true }], pairing: stale });
    let run = startRun('host', before, 0);
    run = observe(run, host({ gen: 6, hosts: before.hosts, pairing: stale })); // an older hub keeps the result while the window is open
    expect(derive(run, host({ gen: 6, hosts: before.hosts, pairing: stale }), 1000).t).toBe('waiting');
    expect(derive(run, state({ gen: 7, hosts: before.hosts, pairing: stale }), 125000)).toEqual({ t: 'failed', result: 'timeout' });
  });
  it('a result is taken only when it changed, gen moved on, and an ok one is in the list', () => {
    const run = observe(startRun('host', state({ gen: 5, pairing: stale, hosts: [{ slot: 0, addr: addrX, transport: 'le', connected: true }] }), 0), host());
    const y = { slot: 1, addr: 'AA:BB:CC:DD:EE:02', transport: 'le' as const, connected: true };
    const x = { slot: 0, addr: addrX, transport: 'le' as const, connected: true };
    const okY = { result: 'ok' as const, addr: y.addr, kind: 'host' as const };
    expect(derive(run, state({ gen: 9, hosts: [x, y], pairing: okY }), 5000)).toMatchObject({ t: 'paired', addr: y.addr });
    expect(derive(run, state({ gen: 4, hosts: [x], pairing: okY }), 5000)).toEqual({ t: 'failed', result: 'timeout' }); // gen did not advance
    expect(derive(run, state({ gen: 9, hosts: [x], pairing: okY }), 5000)).toEqual({ t: 'failed', result: 'timeout' }); // ok for an address the hub does not list
    expect(derive(run, state({ gen: 9, hosts: [x], pairing: { result: 'refused', kind: 'host' } }), 5000)).toEqual({ t: 'failed', result: 'refused' });
  });
});

describe('command ids bound to the action', () => {
  it('a different body or endpoint gets a fresh id even after a timeout', () => {
    const c = new CmdId();
    const a = c.id('pick', { addr: 'A' });
    c.settle(new ApiError(504, 'hub_timeout', 'x'));
    expect(c.id('pick', { addr: 'A' })).toBe(a); // the retry of the same action
    expect(c.id('pick', { addr: 'A' })).toBe(a);
    const b = c.id('pick', { addr: 'B' });
    expect(b).not.toBe(a);
    c.settle(new ApiError(504, 'hub_timeout', 'x'));
    expect(c.id('pick', { addr: 'B' })).toBe(b);
    expect(c.id('forget', { addr: 'B' })).not.toBe(b);
  });
  it('key order of the body does not matter', () => {
    const c = new CmdId();
    expect(c.id('key', { page: 7, usage: 1 })).toBe(c.id('key', { usage: 1, page: 7 }));
  });
});

describe('BtPoll', () => {
  const st = (gen: number, tag = '') => ({ hub: 'h', status: 'ok' as const, state: { ...state({ gen }), tag } as BtState, limits: null, listening: false, age_s: 0 });
  it('keeps one timer and drops a response that was overtaken', async () => {
    vi.useFakeTimers();
    try {
      const pending: ((s: ReturnType<typeof st>) => void)[] = [];
      const api = { btStatus: () => new Promise((res) => pending.push(res as never)) } as never;
      const p = new BtPoll(api, 'h', 1000);
      p.start(); // fetch 1
      const r2 = p.refresh(); // manual refresh while 1 is in flight: fetch 2
      pending[1]!(st(8)); // the newer one answers first
      await r2;
      expect(p.status?.state?.gen).toBe(8);
      pending[0]!(st(7)); // the older one arrives late
      await Promise.resolve();
      await vi.advanceTimersByTimeAsync(0);
      expect(p.status?.state?.gen).toBe(8);
      expect(vi.getTimerCount()).toBe(1); // a single timer
      p.stop();
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });
  it('prefers the higher gen once, then follows a hub that restarted', async () => {
    const seq = [st(9), st(3), st(3)];
    const p = new BtPoll({ btStatus: async () => seq.shift() } as never, 'h');
    await p.refresh();
    await p.refresh();
    expect(p.status?.state?.gen).toBe(9);
    await p.refresh();
    expect(p.status?.state?.gen).toBe(3);
  });
});
