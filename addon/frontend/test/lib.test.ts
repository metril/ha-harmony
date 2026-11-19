import { describe, expect, it } from 'vitest';
import { byteCounter, summarize, utf8Bytes, validateCode } from '../src/lib/codes';
import { locate, summarizeErrors } from '../src/lib/errors';
import { hashFor, hubFromSearch, parseHash } from '../src/lib/router';
import { linkInfo, linkProblem, pickHub } from '../src/lib/hubs';
import type { HubsResponse } from '../src/lib/api';
import { tvConfig } from './helpers';

describe('byte counter', () => {
  it('counts UTF-8 bytes, not characters', () => {
    expect(utf8Bytes('abc')).toBe(3);
    expect(utf8Bytes('é')).toBe(2);
    expect(utf8Bytes('日本')).toBe(6);
    expect(utf8Bytes('😀')).toBe(4);
  });
  it('says how much to shorten', () => {
    expect(byteCounter('a'.repeat(31)).over).toBe(0);
    const c = byteCounter('日本語日本語日本語日本語', 31);
    expect(c.used).toBe(36);
    expect(c.hint).toBe('Shorten by 5 bytes');
    expect(byteCounter('é'.repeat(16)).hint).toBe('Shorten by 1 byte');
  });
});

describe('code validation per protocol', () => {
  const ok = (a: object) => validateCode(a).ok;
  it('enforces the address and command range of each protocol', () => {
    expect(ok({ proto: 'nec', addr: 255, cmd: 255 })).toBe(true);
    expect(validateCode({ proto: 'nec', addr: 256, cmd: 0 }).errors.addr).toMatch(/0\.\.255/);
    expect(ok({ proto: 'necext', addr: 65535, cmd: 65535 })).toBe(true);
    expect(ok({ proto: 'samsung32', addr: 255, cmd: 256 })).toBe(false);
    expect(ok({ proto: 'rc5', addr: 31, cmd: 127 })).toBe(true);
    expect(validateCode({ proto: 'rc5', addr: 32, cmd: 0 }).errors.addr).toBeDefined();
    expect(ok({ proto: 'rc6', addr: 255, cmd: 255 })).toBe(true);
    expect(ok({ proto: 'sirc12', addr: 31, cmd: 128 })).toBe(false);
    expect(ok({ proto: 'sirc15', addr: 255, cmd: 127 })).toBe(true);
    expect(ok({ proto: 'sirc20', addr: 8191, cmd: 127 })).toBe(true);
    expect(ok({ proto: 'sirc20', addr: 8192, cmd: 127 })).toBe(false);
  });
  it('rejects unknown protocols, negatives and non-integers', () => {
    expect(validateCode({ proto: 'x', addr: 1, cmd: 1 }).errors.proto).toBeDefined();
    expect(ok({ proto: 'nec', addr: -1, cmd: 1 })).toBe(false);
    expect(ok({ proto: 'nec', addr: 1.5, cmd: 1 })).toBe(false);
  });
  it('validates pronto structure', () => {
    expect(ok({ pronto: '0000 006D 0001 0000 0018 0018' })).toBe(true);
    expect(validateCode({ pronto: '0000 006D 0002 0000 0018 0018' }).errors.pronto).toMatch(/2\+0/);
    expect(validateCode({ pronto: '0100 006D 0001 0000 0018 0018' }).errors.pronto).toMatch(/0000/);
    expect(validateCode({ pronto: '0000 006D' }).errors.pronto).toMatch(/short/);
    expect(validateCode({ pronto: 'zzzz 006D 0001 0000 0018 0018' }).errors.pronto).toMatch(/hex/);
  });
  it('validates raw timings and port mask', () => {
    expect(ok({ raw: { hz: 38000, us: [9000, 4500, 560, 560] } })).toBe(true);
    expect(validateCode({ raw: { hz: 10, us: [1, 2] } }).errors.hz).toBeDefined();
    expect(validateCode({ raw: { hz: 38000, us: [70000, 1] } }).errors.us).toMatch(/65535/);
    expect(validateCode({ raw: { hz: 38000, us: [0, 1] } }).errors.us).toBeDefined();
    expect(validateCode({ proto: 'nec', addr: 1, cmd: 1, port_mask: 0 }).errors.port_mask).toBeDefined();
    expect(ok({ proto: 'nec', addr: 1, cmd: 1, port_mask: 3 })).toBe(true);
  });
  it('flags kinds the editor cannot edit yet and summarizes codes', () => {
    expect(validateCode({ flipper: { file: 'a.ir' } }).errors.kind).toBeDefined();
    expect(summarize({ proto: 'nec', addr: 4, cmd: 2 })).toBe('nec 4/2');
    expect(summarize({ pronto: '0000 006D 0001 0000 0018 0018' })).toBe('pronto, 6 words');
  });
});

describe('error path to field mapping', () => {
  const cfg = tvConfig();
  it('maps a device command error to its route and field', () => {
    const l = locate({ path: ['devices', 0, 'commands', 'Menü ⏎', 'cmd'], msg: 'x' }, cfg);
    expect(l.route).toBe('devices/tv/commands');
    expect(l.field).toMatch(/^f-devices\.0\.commands\.Men_+\.cmd$/);
  });
  it('routes power, inputs and settings tabs', () => {
    expect(locate({ path: ['devices', 1, 'power', 'on'], msg: '' }, cfg).route).toBe('devices/hdmi_switch/power');
    expect(locate({ path: ['devices', 0, 'inputs', 'hdmi1'], msg: '' }, cfg).route).toBe('devices/tv/inputs');
    expect(locate({ path: ['devices', 0, 'settle_ms'], msg: '' }, cfg).route).toBe('devices/tv/settings');
    expect(locate({ path: ['pages', 0, 'widgets', 0], msg: '' }, cfg).route).toBe('pages');
    expect(locate({ path: ['activities', 0], msg: '' }, cfg).route).toBe('activities');
  });
  it('counts per rail section and keeps unlocated errors for the banner', () => {
    const s = summarizeErrors(
      [
        { path: ['devices', 0, 'id'], msg: 'a' },
        { path: ['devices', 1, 'id'], msg: 'b' },
        { path: ['activities', 0], msg: 'c' },
        { path: [], msg: 'global' },
      ],
      cfg,
    );
    expect(s.bySection).toMatchObject({ devices: 2, activities: 1, overview: 1 });
    expect(s.unlocated.map((u) => u.msg)).toEqual(['global']);
    expect(s.total).toBe(4);
  });
});

describe('router', () => {
  it('parses and builds hash routes', () => {
    expect(parseHash('#/devices/tv/commands')).toEqual({ section: 'devices', parts: ['tv', 'commands'] });
    expect(parseHash('')).toEqual({ section: 'remote', parts: [] });
    expect(parseHash('#/overview').section).toBe('overview');
    expect(parseHash('#/nonsense').section).toBe('remote');
    expect(hashFor('devices', 'a b')).toBe('#/devices/a%20b');
  });
  it('reads the hub from the query and rejects junk', () => {
    expect(hubFromSearch('?hub=sim_hub')).toBe('sim_hub');
    expect(hubFromSearch('?hub=../x')).toBe('default');
    expect(hubFromSearch('')).toBe('default');
  });
});

describe('hub selection', () => {
  const stored = (...ids: string[]) => ids.map((id) => ({ id, has_draft: false, has_applied: true, applied_rev: 'a', applied_at: 1 }));
  const resp = (linked: string[], hubs: string[] = []): HubsResponse => ({ hubs: stored(...hubs), linked: linked.map((id, i) => ({ id, last_seen: 100 - i })), adopt: null });
  it('an explicit ?hub= wins', () => {
    expect(pickHub('?hub=mine', resp(['x'])).hub).toBe('mine');
    expect(pickHub('?hub=mine', resp(['x'])).reason).toBe('explicit');
  });
  it('the one linked hub is used, never default', () => {
    const c = pickHub('', resp(['living-room-hub'], ['default']));
    expect([c.hub, c.reason]).toEqual(['living-room-hub', 'linked']);
  });
  it('several linked hubs: the newest, with every id in the switcher', () => {
    const c = pickHub('', resp(['b', 'a'], ['default']));
    expect([c.hub, c.reason, c.choices]).toEqual(['b', 'newest-linked', ['b', 'a', 'default']]);
  });
  it('several linked hubs: the remembered one wins when still linked', () => {
    expect(pickHub('', resp(['b', 'a']), 'a').hub).toBe('a');
    expect(pickHub('', resp(['b', 'a']), 'gone').hub).toBe('b');
  });
  it('nothing linked: a stored hub, else default, and anyLinked is false', () => {
    expect(pickHub('', resp([], ['sim'])).hub).toBe('sim');
    const c = pickHub('', resp([], ['default']));
    expect([c.hub, c.anyLinked]).toEqual(['default', false]);
    expect(pickHub('?hub=../x', null).hub).toBe('default');
  });
  it('link info: none, other, stale, polling', () => {
    const s = (linked_hubs: { id: string; last_seen: number }[]) => ({ linked_hubs, now: 1000 }) as never;
    expect(linkInfo(s([]), 'h').kind).toBe('none');
    expect(linkInfo(s([{ id: 'x', last_seen: 999 }]), 'h').kind).toBe('other');
    expect(linkInfo(s([{ id: 'h', last_seen: 500 }]), 'h').kind).toBe('stale');
    expect(linkInfo(s([{ id: 'h', last_seen: 990 }]), 'h').kind).toBe('polling');
    expect(linkInfo({} as never, 'h').kind).toBe('unknown');
    expect(linkProblem(linkInfo(s([{ id: 'x', last_seen: 999 }]), 'h'), 'h')).toContain('not polling for this hub (h)');
    expect(linkProblem({ kind: 'polling', ids: [] }, 'h')).toBeNull();
  });
});
