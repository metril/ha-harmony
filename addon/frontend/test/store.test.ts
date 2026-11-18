import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Store } from '../src/lib/store';
import { META, USAGE, fakeApi, previewBad, previewOk, tvConfig, type Handler } from './helpers';

const draftResp = { hub: 'h', draft_updated: 1_700_000_100, ...META };
function setup(over: Record<string, Handler> = {}, draft: unknown = null) {
  const puts: unknown[] = [];
  const f = fakeApi({
    'GET api/v1/config': () => ({ body: { hub: 'h', draft, ...META, applied: { ...META.applied, config: tvConfig() } } }),
    'PUT api/v1/config/draft': (c) => {
      puts.push(JSON.parse(c.body ?? '{}').config);
      return { body: draftResp };
    },
    'POST api/v1/config/preview': () => ({ body: previewOk({ usage: { ...USAGE, json_bytes: 2048 } }) }),
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

  it('keeps the edits and retries when the add-on is unreachable (restart), then says it is back', async () => {
    let up = false;
    const puts: unknown[] = [];
    const { store } = setup({
      'PUT api/v1/config/draft': (c) => {
        if (!up) return { status: 503, body: { error: { code: 'unavailable', msg: 'starting' } } };
        puts.push(JSON.parse(c.body ?? '{}').config);
        return { body: draftResp };
      },
    });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Kept'));
    await vi.advanceTimersByTimeAsync(700);
    expect(store.saveState).toBe('error');
    expect(store.offline).toBe(true);
    expect(store.config.devices![0]!.name).toBe('Kept'); // nothing rolled back
    expect(store.toasts.filter((t) => t.kind === 'error')).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(2100); // first retry, still down: no second toast
    expect(store.toasts.filter((t) => t.kind === 'error')).toHaveLength(1);
    up = true;
    await vi.advanceTimersByTimeAsync(4200);
    expect(puts).toHaveLength(1);
    expect((puts[0] as { devices: { name: string }[] }).devices[0]?.name).toBe('Kept');
    expect(store.saveState).toBe('saved');
    expect(store.offline).toBe(false);
    expect(store.toasts.some((t) => /back/.test(t.text))).toBe(true);
  });

  it('keeps a discarded draft one Undo away', async () => {
    const draft = { config: { ...tvConfig(), keys: { play: { none: true } } }, updated: 1_700_000_000 };
    const { store } = setup({ 'POST api/v1/config/revert': () => ({ body: { ...META, hub: 'h' } }) }, draft);
    await store.load();
    expect(store.config.keys).toEqual({ play: { none: true } });
    await store.discard();
    expect(store.toasts.some((t) => t.undo)).toBe(true);
    expect(store.canUndo).toBe(true);
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
      'PUT api/v1/config/draft': () => (conflict ? { status: 409, body: { error: { code: 'conflict', msg: 'stale' } } } : { body: { ...draftResp, rev: 9 } }),
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

  it('keeps the errors a 200 preview reports, and drops the meters', async () => {
    const errors = [{ path: ['devices', 0, 'commands', 'x'], msg: 'bad', line: null }];
    const { store } = setup({ 'POST api/v1/config/preview': () => ({ body: previewBad(errors) }) });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'Z'));
    await vi.advanceTimersByTimeAsync(700);
    expect(store.errors).toEqual(errors);
    expect(store.usage).toBeNull();
    expect(store.errorSummary.bySection['devices']).toBe(1);
  });

  it('sends the rev from GET config as If-Match, then the rev each PUT returned', async () => {
    let n = 7;
    const { store, calls } = setup({ 'PUT api/v1/config/draft': () => ({ body: { ...draftResp, rev: ++n } }) });
    await store.load();
    store.edit((c) => (c.devices![0]!.name = 'A'));
    await vi.advanceTimersByTimeAsync(700);
    store.edit((c) => (c.devices![0]!.name = 'B'));
    await vi.advanceTimersByTimeAsync(700);
    const puts = calls.filter((c) => c.method === 'PUT').map((c) => c.headers['If-Match']);
    expect(puts).toEqual(['7', '8']);
  });

  it('restores a history version as the draft from GET config/history', async () => {
    const old = { ...tvConfig(), activities: [] };
    const { store, puts, calls } = setup({ 'GET api/v1/config/history': () => ({ body: { hub: 'h', rev: 2, at: 1, hash: 'x', config: old } }) });
    await store.load();
    await store.restoreAsDraft(2);
    expect(calls.find((c) => c.url.includes('config/history'))?.url).toContain('rev=2');
    expect(puts.at(-1)).toEqual(old);
    expect(store.hasDraft).toBe(true);
  });

  it('previews the applied config explicitly when there is no draft (the add-on would answer 409 no_draft)', async () => {
    const { store, calls } = setup();
    await store.load();
    const body = JSON.parse(calls.find((c) => c.url.includes('config/preview'))!.body!);
    expect(body.config.devices).toHaveLength(2);
    expect(store.usage?.json_bytes).toBe(2048);
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
    const { store } = setup({ 'GET api/v1/config': () => ({ body: { hub: 'h', draft: null, applied: null, history: [], history_max: 5, rev: 0 } }) });
    await store.load();
    expect(store.isFirstRun).toBe(true);
  });
});
