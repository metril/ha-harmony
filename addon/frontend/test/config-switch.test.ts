import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Store, nameFromFile, setStore } from '../src/lib/store';
import { fakeLive } from './fake-ws';
import { PUTRESP, fakeApi, libRoutes, previewOk, tvConfig, type Handler } from './helpers';
import '../src/components/hh-app';

const sum = (id: string, name: string) => ({ id, name, created: 1, updated: 2, latest_rev: null, rev_count: 0, draft_rev: 4, has_draft: true, draft_updated: 1, base_rev: null, dirty: false });
const dCfg = () => ({ ...tvConfig(), devices: [{ ...tvConfig().devices![0]!, id: 'other', name: 'Other TV' }] });

function setup(over: Record<string, Handler> = {}) {
  const puts: { id: string; config: unknown }[] = [];
  const f = fakeApi({
    ...libRoutes({ config: tvConfig() }),
    'GET api/v1/configs/d': () => ({ body: { ...sum('d', 'Second'), hubs: [], revs: [], draft: { config: dCfg(), updated: 1, base_rev: null } } }),
    'GET api/v1/configs': () => ({ body: { configs: [sum('c', 'Main'), sum('d', 'Second')] } }),
    'PUT api/v1/configs/c/draft': (c) => (puts.push({ id: 'c', config: JSON.parse(c.body ?? '{}').config }), { body: PUTRESP }),
    'PUT api/v1/configs/d/draft': (c) => (puts.push({ id: 'd', config: JSON.parse(c.body ?? '{}').config }), { body: { ...PUTRESP, id: 'd' } }),
    'POST api/v1/config/preview': () => ({ body: previewOk() }),
    ...over,
  });
  const store = new Store(f.api, 'h', 'c', 600);
  return { store, puts, ...f };
}
beforeEach(() => {
  vi.useFakeTimers();
  sessionStorage.clear();
});
afterEach(() => vi.useRealTimers());

describe('switching config in place', () => {
  it('saves the old edits to the old config and shows the new one with no bleed', async () => {
    const { store, puts } = setup();
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Edited'));
    expect(store.canUndo).toBe(true);
    await store.switchTo('d');
    expect(puts).toHaveLength(1);
    expect(puts[0]!.id).toBe('c');
    expect(store.configId).toBe('d');
    expect(store.configName).toBe('Second');
    expect(store.config.devices![0]!.name).toBe('Other TV');
    expect(store.canUndo).toBe(false);
    expect(store.canRedo).toBe(false);
    expect(store.saveState).toBe('clean');
    expect(store.revs).toEqual([]);
    expect(store.baseline).toBeNull();
    // a late save of the old config must not write to the new one
    await vi.advanceTimersByTimeAsync(2000);
    expect(puts.filter((p) => p.id === 'd')).toHaveLength(0);
    // edits now go to the new config's own draft queue
    store.edit((c) => (c.devices![0]!.name = 'D2'));
    await vi.advanceTimersByTimeAsync(700);
    expect(puts.at(-1)!.id).toBe('d');
  });

  it('stashes edits that could not be saved under the old config key', async () => {
    const { store } = setup({ 'PUT api/v1/configs/c/draft': () => ({ status: 503, body: { error: { code: 'unavailable', msg: 'x' } } }) });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Kept'));
    await store.switchTo('d');
    const raw = JSON.parse(sessionStorage.getItem('hh-unsaved:c')!);
    expect(raw.config.devices[0].name).toBe('Kept');
    expect(sessionStorage.getItem('hh-unsaved:d')).toBeNull();
    expect(store.stashed).toBeNull();
  });

  it('imports YAML as a new config and switches to it', async () => {
    const { store, calls } = setup({
      'POST api/v1/configs': () => ({ status: 201, body: { ok: true, ...sum('d', 'My Home'), hubs: [], revs: [], draft: { config: dCfg(), updated: 1, base_rev: null } } }),
    });
    await store.load();
    await store.importAsNew('My Home', 'devices: []');
    expect(JSON.parse(calls.find((c) => c.method === 'POST' && c.url.endsWith('api/v1/configs'))!.body!)).toEqual({ name: 'My Home', yaml: 'devices: []' });
    expect(store.configId).toBe('d');
    expect(store.config.devices![0]!.name).toBe('Other TV');
  });

  it('names an imported file in Title Case', () => {
    expect(nameFromFile('my-home_setup.yaml')).toBe('My Home Setup');
    expect(nameFromFile('.yaml')).toBe('Imported Config');
  });
});

describe('config deleted elsewhere', () => {
  const gone: Record<string, Handler> = { 'GET api/v1/configs/c': () => ({ status: 404, body: { error: { code: 'not_found', msg: 'gone' } } }) };

  it('stops autosave and keeps the edit stashed', async () => {
    let missing = false;
    const { store, puts } = setup({ 'GET api/v1/configs/c': (c) => (missing ? gone['GET api/v1/configs/c']!(c) : libRoutes({ config: tvConfig() })['GET api/v1/configs/c']!(c)) });
    await store.load();
    missing = true;
    store.edit((c) => (c.devices![0]!.name = 'Mine'));
    await store.checkDeleted();
    expect(store.deleted).toBe(true);
    await vi.advanceTimersByTimeAsync(2000);
    expect(puts).toHaveLength(0);
    expect(JSON.parse(sessionStorage.getItem('hh-unsaved:c')!).config.devices[0].name).toBe('Mine');
  });

  it('Save As New Config creates it from the draft and opens it', async () => {
    let missing = false;
    const { store, puts } = setup({
      'GET api/v1/configs/c': (c) => (missing ? gone['GET api/v1/configs/c']!(c) : libRoutes({ config: tvConfig() })['GET api/v1/configs/c']!(c)),
      'POST api/v1/configs': () => ({ status: 201, body: { ok: true, ...sum('d', 'Main Restored'), hubs: [], revs: [], draft: null } }),
    });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Mine'));
    await store.flush();
    missing = true;
    store.edit((c) => (c.devices![0]!.name = 'Mine2'));
    await store.checkDeleted();
    await store.saveAsNew('Main Restored');
    expect(puts.at(-1)).toEqual({ id: 'd', config: expect.objectContaining({ devices: expect.any(Array) }) });
    expect((puts.at(-1)!.config as { devices: { name: string }[] }).devices[0]!.name).toBe('Mine2');
    expect(store.configId).toBe('d');
    expect(store.deleted).toBe(false);
  });

  it('shows the banner when the configs push drops the open config', async () => {
    let missing = false;
    const { store } = setup({ 'GET api/v1/configs/c': (c) => (missing ? gone['GET api/v1/configs/c']!(c) : libRoutes({ config: tvConfig() })['GET api/v1/configs/c']!(c)) });
    fakeLive.topics['hubs'] = { adopt: null, now: 1000, hubs: [], linked: [] };
    fakeLive.topics['configs'] = { configs: [sum('c', 'Main'), sum('d', 'Second')] };
    setStore(store);
    await store.load();
    const app = document.createElement('hh-app') as HTMLElement & { updateComplete: Promise<unknown> };
    document.body.append(app);
    await app.updateComplete;
    expect(app.shadowRoot!.querySelector('#config-deleted')).toBeNull();
    missing = true;
    fakeLive.push({ t: 'configs', configs: [sum('d', 'Second')] });
    await vi.advanceTimersByTimeAsync(50);
    await app.updateComplete;
    const b = app.shadowRoot!.querySelector('#config-deleted')!;
    expect(b.textContent).toContain('This Config Was Deleted');
    expect(b.textContent).toContain('Save As New Config');
    expect(b.textContent).toContain('Open Another Config');
    app.remove();
  });
});
