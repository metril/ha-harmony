// The editor's one WebSocket: refcounted topics, snapshot cache, resync, back-off, wake-up events, and no network while idle.
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Api } from '../src/lib/api';
import { Live } from '../src/lib/live';
import { fakeLive } from './fake-ws';

const tick = () => new Promise((r) => setTimeout(r, 0));
const mk = () => new Live('wss://ha.example/api/hassio_ingress/tok/api/v1/live');
afterEach(() => vi.useRealTimers());

describe('Live', () => {
  it('derives the ws/wss url relative to the ingress path', () => {
    new Api('https://ha.example/api/hassio_ingress/tok/').live.sub('hubs', () => undefined);
    expect(fakeLive.sockets[0]!.url).toBe('wss://ha.example/api/hassio_ingress/tok/api/v1/live');
    new Api('http://lan.example/x/').live.sub('hubs', () => undefined);
    expect(fakeLive.sockets[1]!.url).toBe('ws://lan.example/x/api/v1/live');
  });

  it('opens one socket for any number of subscribers and sends the whole topic set', async () => {
    const l = mk();
    const got: unknown[] = [];
    const u1 = l.sub('hub:h', (v) => got.push(v));
    l.sub('bt:h', () => undefined);
    fakeLive.topics['hub:h'] = { hub: 'h', acked_rev: null };
    await tick();
    expect(fakeLive.sockets).toHaveLength(1);
    expect(fakeLive.sockets[0]!.sent.at(-1)).toEqual({ sub: ['hub:h', 'bt:h'] });
    // a second subscriber of a known topic gets the cached snapshot and costs no frame
    fakeLive.push({ t: 'snapshot', topics: { 'hub:h': { hub: 'h', acked_rev: 'a' } } });
    const sent = fakeLive.sockets[0]!.sent.length;
    const late: unknown[] = [];
    l.sub('hub:h', (v) => late.push(v));
    expect(late).toEqual([{ hub: 'h', acked_rev: 'a' }]);
    expect(fakeLive.sockets[0]!.sent.length).toBe(sent);
    fakeLive.push({ t: 'hub', hub: 'h', acked_rev: 'b' });
    expect(late.at(-1)).toEqual({ hub: 'h', acked_rev: 'b' });
    u1();
  });

  it('closes the socket when the last subscriber leaves, and stays closed', async () => {
    vi.useFakeTimers();
    const l = mk();
    const u = l.sub('hubs', () => undefined);
    await vi.advanceTimersByTimeAsync(0);
    u();
    expect(fakeLive.open).toHaveLength(0);
    await vi.advanceTimersByTimeAsync(120_000);
    expect(fakeLive.sockets).toHaveLength(1);
  });

  it('a resync drops the cached topic until its snapshot arrives', async () => {
    const l = mk();
    l.sub('bt:h', () => undefined);
    await tick();
    fakeLive.push({ t: 'bt', hub: 'h', status: 'ok' });
    expect(l.get('bt:h')).toEqual({ hub: 'h', status: 'ok' });
    fakeLive.push({ t: 'resync', topic: 'bt:h' });
    expect(l.get('bt:h')).toBeUndefined();
    fakeLive.push({ t: 'snapshot', topics: { 'bt:h': { hub: 'h', status: 'offline' } } });
    expect(l.get('bt:h')).toEqual({ hub: 'h', status: 'offline' });
  });

  it('key frames are delivered one by one and never cached; unsubscribed topics are ignored', async () => {
    const l = mk();
    const keys: unknown[] = [];
    l.sub('keys:h', (v) => keys.push(v));
    await tick();
    fakeLive.push({ t: 'key', hub: 'h', seq: 1, slot: 0, page: 7, usage: 4, down: true });
    fakeLive.push({ t: 'hub', hub: 'zzz', acked_rev: 'x' });
    expect(keys).toHaveLength(1);
    expect(l.get('keys:h')).toBeUndefined();
    expect(l.get('hub:zzz')).toBeUndefined();
  });

  it('reconnects with a jittered back-off from 1 s up to 30 s and re-sends the subscription', async () => {
    vi.useFakeTimers();
    const l = mk();
    l.sub('hubs', () => undefined);
    await vi.advanceTimersByTimeAsync(0);
    const waits: number[] = [];
    for (let i = 0; i < 8; i++) {
      const n = fakeLive.sockets.length;
      fakeLive.auto = false; // the next attempts never open
      fakeLive.drop();
      let t = 0;
      while (fakeLive.sockets.length === n && t < 40_000) {
        await vi.advanceTimersByTimeAsync(100);
        t += 100;
      }
      waits.push(t);
      expect(fakeLive.sockets.length).toBe(n + 1);
      fakeLive.sockets.at(-1)!.onclose?.(); // it failed to connect
      fakeLive.sockets.at(-1)!.closed = true;
    }
    expect(Math.min(...waits)).toBeGreaterThanOrEqual(1000);
    expect(Math.max(...waits)).toBeLessThanOrEqual(30_100);
    expect(waits.at(-1)!).toBeGreaterThan(waits[0]!);
    // it opens again: the subscription is re-sent
    fakeLive.auto = true;
    await vi.advanceTimersByTimeAsync(40_000);
    expect(fakeLive.open.at(-1)?.sent.at(-1)).toEqual({ sub: ['hubs'] });
    expect(l.up).toBe(true);
  });

  it('visibilitychange and online reconnect at once, no timer needed', async () => {
    vi.useFakeTimers();
    const l = mk();
    l.sub('hubs', () => undefined);
    await vi.advanceTimersByTimeAsync(0);
    fakeLive.auto = false;
    fakeLive.drop();
    expect(l.up).toBe(false);
    const n = fakeLive.sockets.length;
    window.dispatchEvent(new Event('online'));
    expect(fakeLive.sockets.length).toBe(n + 1);
  });
});

describe('idle', () => {
  it('nothing subscribed: no socket, no fetch, no XHR, no timer', async () => {
    vi.useFakeTimers();
    const f = vi.fn();
    const xhr = vi.spyOn(XMLHttpRequest.prototype, 'send');
    const api = new Api('https://ha.example/x/', f as never);
    void api.live;
    await vi.advanceTimersByTimeAsync(10 * 60_000);
    expect(f).not.toHaveBeenCalled();
    expect(xhr).not.toHaveBeenCalled();
    expect(fakeLive.sockets).toHaveLength(0);
    expect(vi.getTimerCount()).toBe(0);
  });
});
