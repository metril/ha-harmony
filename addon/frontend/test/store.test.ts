import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Store } from '../src/lib/store';
import { META, fakeApi, tvConfig } from './helpers';

const draftResp = { hub: 'h', draft_updated: 1_700_000_100, ...META };
function setup(over: Record<string, () => { status?: number; body?: unknown }> = {}, draft: unknown = null) {
  const puts: unknown[] = [];
  const f = fakeApi({
    'GET api/v1/config': () => ({ body: { hub: 'h', draft, ...META, applied: { ...META.applied, config: tvConfig() } } }),
    'PUT api/v1/config/draft': (c) => {
      puts.push(JSON.parse(c.body ?? '{}').config);
      return { body: draftResp };
    },
    'POST api/v1/config/preview': () => ({ body: { ok: true, hash: 'h1', pages: [], usage: { json_bytes: 2048, json_max: 131072, pool_words: 100, pool_max: 1000, devices: 2, devices_max: 16, activities: 1, activities_max: 16 } } }),
    ...over,
  });
  const store = new Store(f.api, 'h', 600);
  return { store, puts, ...f };
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('draft store', () => {
  it('debounces edits into one PUT about 600 ms after the last change', async () => {
    const { store, puts } = setup();
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'A'));
    await vi.advanceTimersByTimeAsync(400);
    store.edit((c) => (c.devices![0]!.name = 'AB'));
    await vi.advanceTimersByTimeAsync(599);
    expect(puts).toHaveLength(0);
    expect(store.saveState).toBe('dirty');
    await vi.advanceTimersByTimeAsync(2);
    expect(puts).toHaveLength(1);
    expect((puts[0] as { devices: { name: string }[] }).devices[0]?.name).toBe('AB');
    expect(store.saveState).toBe('saved');
    expect(store.usage?.json_bytes).toBe(2048);
    expect(store.hasDraft).toBe(true);
  });

  it('shows an optimistic edit at once and rolls back with a toast when the PUT fails', async () => {
    const { store } = setup({ 'PUT api/v1/config/draft': () => ({ status: 500, body: { error: { code: 'x', msg: 'disk full' } } }) });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Changed'));
    expect(store.config.devices![0]!.name).toBe('Changed');
    await vi.advanceTimersByTimeAsync(700);
    expect(store.config.devices![0]!.name).toBe('Living room TV');
    expect(store.hasDraft).toBe(false);
    expect(store.saveState).toBe('error');
    expect(store.toasts[0]?.kind).toBe('error');
  });

  it('enters the conflict state on 409 and keeps local edits; Overwrite resends', async () => {
    let conflict = true;
    const { store, calls } = setup({
      'PUT api/v1/config/draft': () => (conflict ? { status: 409, body: { error: { code: 'conflict', msg: 'stale' } } } : { body: { ...draftResp, draft_rev: 9 } }),
    });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Mine'));
    await vi.advanceTimersByTimeAsync(700);
    expect(store.saveState).toBe('conflict');
    expect(store.config.devices![0]!.name).toBe('Mine');
    conflict = false;
    await store.overwrite();
    expect(store.saveState).toBe('saved');
    expect(calls.filter((c) => c.method === 'PUT')).toHaveLength(2);
  });

  it('keeps validation errors from a 422 preview and blocks nothing else', async () => {
    const errors = [{ path: ['devices', 0, 'commands', 'x'], msg: 'bad' }];
    const { store } = setup({ 'POST api/v1/config/preview': () => ({ status: 422, body: { ok: false, errors } }) });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Z'));
    await vi.advanceTimersByTimeAsync(700);
    expect(store.errors).toEqual(errors);
    expect(store.errorSummary.bySection['devices']).toBe(1);
  });

  it('computes the diff against the applied config', async () => {
    const { store } = setup();
    await store.load();
    store.edit((c) => {
      delete c.devices![0]!.commands!['cmd_0'];
      c.devices!.push({ id: 'new', name: 'New' });
      c.activities = [];
    });
    const kinds = store.diff.map((d) => `${d.kind}:${d.area}:${d.label}`);
    expect(kinds).toContain('changed:Device:Living room TV');
    expect(kinds).toContain('added:Device:New');
    expect(kinds).toContain('removed:Activity:Watch TV');
  });

  it('is first-run when nothing is applied and there is no draft', async () => {
    const { store } = setup({ 'GET api/v1/config': () => ({ body: { hub: 'h', draft: null, applied: null, history: [], history_max: 5 } }) });
    await store.load();
    expect(store.isFirstRun).toBe(true);
  });
});
