// Lifecycle and wiring of the Bluetooth views: leaving mid-request, unmappable presses, names, live regions, palette counts, priming.
import '../src/views/hh-bt-keys';
import '../src/views/hh-bt-pair';
import '../src/views/hh-bt-remote';
import '../src/views/hh-bt-panel';
import '../src/components/hh-palette';
import { beforeEach, describe, expect, it } from 'vitest';
import { Store, setStore } from '../src/lib/store';
import { setBtDefaults } from '../src/lib/bt';
import { StudioState } from '../src/lib/studio-state';
import { Assigner } from '../src/lib/assigner';
import type { BtCodes, BtStatus } from '../src/lib/api';
import type { Config } from '../src/lib/model';
import { META, fakeApi, type Call, type Handler } from './helpers';

type El = HTMLElement & { updateComplete: Promise<unknown>; shadowRoot: ShadowRoot };
const codes: BtCodes = {
  builtins: [
    { name: 'Home', page: 12, usage: 0x223, default: true },
    { name: 'Select', page: 7, usage: 0x28, default: true },
    { name: 'Back', page: 12, usage: 0x224, default: false },
  ],
  remote_keys: [{ name: 'Home', page: 12, usage: 0x223, key: 'home' }],
  keys: ['up', 'home', 'menu'],
};
const status = (o: Partial<BtStatus> = {}): BtStatus => ({
  hub: 'h',
  status: 'ok',
  state: { state: 'ready', discoverable: false, hosts: [], remotes: [], gen: 1 },
  limits: { hosts_max: 8, conn_max: 3, remotes_max: 4, le_bonds_max: 8, pages: [7, 12], le: true },
  listening: false,
  age_s: 0,
  ...o,
});
const gate = () => {
  let open!: () => void;
  const p = new Promise<void>((r) => (open = r));
  return { p, open };
};
const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));
const bodies = (calls: Call[], path: string) => calls.filter((c) => c.url.includes(path)).map((c) => JSON.parse(c.body ?? '{}') as Record<string, unknown>);

async function boot(routes: Record<string, Handler> = {}, cfg: Config = {}) {
  const f = fakeApi({
    'GET api/v1/config': () => ({ body: { hub: 'h', draft: { config: cfg, updated: 1 }, ...META, applied: null } }),
    'POST api/v1/config/preview': () => ({ body: { ok: true, errors: [], usage: null } }),
    'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 2, ...META } }),
    'GET api/v1/hubs': () => ({ body: { hubs: [{ id: 'h', has_draft: true, has_applied: false, applied_rev: null, applied_at: null }] } }),
    'GET api/v1/bt/status': () => ({ body: status() }),
    'GET api/v1/bt/codes': () => ({ body: codes }),
    'POST api/v1/bt/pair/stop': () => ({ body: { ok: true } }),
    'POST api/v1/bt/listen': () => ({ body: { ok: true, listening: true } }),
    'GET api/v1/bt/events': () => ({ body: { events: [], last: 0, listening: true } }),
    ...routes,
  });
  const store = new Store(f.api, 'h', 600);
  setStore(store);
  await store.load();
  return { store, ...f };
}
const mount = async <T extends El>(tag: string, props: Record<string, unknown> = {}): Promise<T> => {
  const el = document.createElement(tag) as T;
  Object.assign(el, props);
  document.body.append(el);
  await el.updateComplete;
  return el;
};

beforeEach(() => {
  document.body.replaceChildren();
  setBtDefaults([]);
});

describe('hh-bt-keys', () => {
  it('leaving while learn is starting turns the listen lease off and polls nothing', async () => {
    const g = gate();
    const { calls, store } = await boot({ 'POST api/v1/bt/listen': async (c) => { if (JSON.parse(c.body ?? '{}').on) await g.p; return { body: { ok: true, listening: true } }; } });
    const el = await mount('hh-bt-keys', { api: store.api, hub: 'h', codes });
    el.shadowRoot.querySelector<HTMLButtonElement>('.learn .btn.pri')!.click();
    await tick();
    el.remove();
    g.open();
    await tick(60);
    const listens = bodies(calls, 'bt/listen').map((b) => b['on']);
    expect(listens[0]).toBe(true);
    expect(listens.at(-1)).toBe(false);
    expect(calls.filter((c) => c.url.includes('bt/events'))).toHaveLength(0);
    const n = calls.length;
    await tick(100); // nothing scheduled
    expect(calls).toHaveLength(n);
  });

  it('a press on a page the hub cannot map is explained and cannot be assigned', async () => {
    const { store } = await boot();
    const el = await mount<El & { pressed: unknown; seen: unknown[] }>('hh-bt-keys', { api: store.api, hub: 'h', codes, pages: [7, 12] });
    const changes: unknown[] = [];
    el.addEventListener('change', (e) => changes.push((e as CustomEvent).detail));
    el.pressed = { page: 1, usage: 0x30 };
    await el.updateComplete;
    const got = el.shadowRoot.querySelector('.got')!;
    expect(got.textContent).toContain('cannot map');
    expect(got.querySelector('select, button')).toBeNull();
    el.pressed = { page: 12, usage: 0x30 };
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.got select')).not.toBeNull();
    expect(changes).toHaveLength(0);
  });
});

describe('hh-bt-pair', () => {
  it('leaving while the window is being opened closes it', async () => {
    const g = gate();
    const { calls } = await boot({ 'POST api/v1/bt/pair': async () => { await g.p; return { body: { ok: true, state: null } }; } });
    const el = await mount('hh-bt-pair', { mode: 'host' });
    await vi_waitFor(() => !!el.shadowRoot.querySelector('.stage .btn.pri:not([disabled])'));
    el.shadowRoot.querySelector<HTMLButtonElement>('.stage .btn.pri')!.click();
    await tick();
    el.remove();
    expect(calls.filter((c) => c.url.includes('pair/stop'))).toHaveLength(0);
    g.open();
    await tick(40);
    expect(calls.filter((c) => c.url.includes('pair/stop'))).toHaveLength(1);
  });

  it('the pairing wizard keeps the countdown out of every live region', async () => {
    await boot({ 'GET api/v1/bt/status': () => ({ body: status({ state: { state: 'ready', discoverable: true, discoverable_secs: 90, window: 'host', hosts: [], remotes: [], gen: 2 } }) }) });
    const el = await mount('hh-bt-pair', { mode: 'host' });
    await tick(30);
    for (const live of el.shadowRoot.querySelectorAll('[role="status"], [aria-live="polite"], [role="alert"]')) expect(live.textContent).not.toMatch(/\d:\d\d/);
  });
});

describe('hh-bt-panel', () => {
  it('the open-window banner announces the window, not the seconds', async () => {
    await boot({ 'GET api/v1/bt/status': () => ({ body: status({ state: { state: 'ready', discoverable: true, discoverable_secs: 90, window: 'host', hosts: [], remotes: [], gen: 2 } }) }) });
    const el = await mount('hh-bt-panel');
    await tick(30);
    const live = el.shadowRoot.querySelector('.banner [role="status"]')!;
    expect(live.textContent).toContain('pairing window');
    expect(live.textContent).not.toMatch(/\d:\d\d/);
    const t = el.shadowRoot.querySelector('.banner [role="timer"]')!;
    expect(t.getAttribute('aria-live')).toBe('off');
    expect(t.textContent).toMatch(/\d:\d\d left/);
    expect(el.shadowRoot.querySelector('.banner[role="status"]')).toBeNull();
  });
});

describe('hh-bt-remote', () => {
  it('refuses a name another remote already has', async () => {
    const cfg: Config = { bt_remotes: [{ name: 'Lounge', address: 'AA:BB:CC:00:00:01' }, { name: 'Den', address: 'AA:BB:CC:00:00:02' }] } as Config;
    const { store } = await boot({}, cfg);
    const el = await mount('hh-bt-remote', { index: 1 });
    const input = el.shadowRoot.querySelector<HTMLInputElement>('input[aria-label="Remote name"]')!;
    input.value = 'Lounge';
    input.dispatchEvent(new Event('change'));
    await el.updateComplete;
    expect(el.shadowRoot.querySelector('.err')?.textContent).toContain('Another remote');
    expect((store.config['bt_remotes'] as { name: string }[])[1]!.name).toBe('Den');
    input.value = 'Bedroom';
    input.dispatchEvent(new Event('change'));
    await el.updateComplete;
    expect((store.config['bt_remotes'] as { name: string }[])[1]!.name).toBe('Bedroom');
    expect(el.shadowRoot.querySelector('.err')).toBeNull();
  });
});

describe('store and palette with a default-set Bluetooth device', () => {
  const box = { id: 'box', kind: 'bt', name: 'Box', address: 'AA:BB:CC:DD:EE:FF' };
  it('primes the default commands when a config with one is put in place after load', async () => {
    const { store, calls } = await boot();
    expect(calls.filter((c) => c.url.includes('bt/codes'))).toHaveLength(0);
    store.replace({ devices: [box] } as Config);
    await vi_waitFor(() => calls.some((c) => c.url.includes('bt/codes')));
    await tick();
    const { commandNames } = await import('../src/lib/bt');
    expect(commandNames(box as never)).toEqual(['Home', 'Select']);
  });
  it('the palette counts and searches the default commands', async () => {
    const { store } = await boot({}, { devices: [box] } as Config);
    await vi_waitFor(async () => (await import('../src/lib/bt')).commandNames(box as never).length === 2);
    const el = await mount('hh-palette', { host: { state: new StudioState(store) }, assigner: new Assigner(() => undefined) });
    await tick();
    expect(el.shadowRoot.querySelector('details summary small')?.textContent).toBe('2 cmd');
  });
});

async function vi_waitFor(f: () => boolean | Promise<boolean>, ms = 1500) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (await f()) return;
    await tick(10);
  }
  throw new Error('timeout waiting');
}
