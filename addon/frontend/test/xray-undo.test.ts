import { describe, expect, it } from 'vitest';
import keymap from './fixtures/keymap.json';
import { KEYS } from '../src/lib/remote-layout';
import { xrayLabels } from '../src/lib/xray';
import { Store } from '../src/lib/store';
import { META, fakeApi, previewOk, tvConfig } from './helpers';

describe('x-ray labels', () => {
  const all = KEYS.map((k) => ({ key: k.name, text: `label of ${k.name}` }));
  it('labels every key with text once, on the side it sits on', () => {
    const l = xrayLabels(all);
    expect(l).toHaveLength(35);
    expect(new Set(l.map((x) => x.key)).size).toBe(35);
    expect(l.find((x) => x.key === 'volume_up')?.side).toBe('left');
    expect(l.find((x) => x.key === 'channel_up')?.side).toBe('right');
  });
  it('keeps labels of one column apart, in y order', () => {
    const l = xrayLabels(all);
    for (const side of ['left', 'right'] as const) {
      const ys = l.filter((x) => x.side === side).map((x) => x.y);
      ys.slice(1).forEach((y, i) => expect(y - (ys[i] as number)).toBeGreaterThanOrEqual(21.9));
    }
  });
  it('skips keys with no action text and uses the backend key list', () => {
    const rows = keymap.default.map((r) => ({ key: r.key, text: r.action?.label ?? '' }));
    expect(xrayLabels(rows).map((x) => x.key).sort()).toEqual(keymap.default.filter((r) => r.action).map((r) => r.key).sort());
  });
});

describe('undo history', () => {
  const mk = async () => {
    const f = fakeApi({
      'GET api/v1/config': () => ({ body: { hub: 'h', draft: { config: tvConfig(), updated: 1 }, ...META } }),
      'POST api/v1/config/preview': () => ({ body: previewOk() }),
      'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 2, ...META } }),
    });
    const s = new Store(f.api, 'h', 5000);
    await s.load();
    return s;
  };
  it('one step per edit, redo restores, and a new edit clears redo', async () => {
    const s = await mk();
    expect(s.canUndo).toBe(false);
    s.edit((c) => (c.keys = { ...c.keys, red: { activity: 'off' } }));
    s.edit((c) => (c.keys = { ...c.keys, blue: { activity: 'off' } }));
    expect(Object.keys(s.config.keys ?? {})).toEqual(['volume_up', 'red', 'blue']);
    expect(s.undo()).toBe(true);
    expect(Object.keys(s.config.keys ?? {})).toEqual(['volume_up', 'red']);
    expect(s.canRedo).toBe(true);
    s.redo();
    expect(Object.keys(s.config.keys ?? {})).toEqual(['volume_up', 'red', 'blue']);
    s.undo();
    s.edit((c) => (c.keys = {}));
    expect(s.canRedo).toBe(false);
    s.undo();
    s.undo();
    expect(s.undo()).toBe(false);
    expect(Object.keys(s.config.keys ?? {})).toEqual(['volume_up']);
  });
  it('edits sharing a coalesce key (typing) are a single undo step', async () => {
    const s = await mk();
    for (const t of ['a', 'ab', 'abc']) s.edit((c) => (c.devices![0]!.name = t), 'name');
    s.undo();
    expect(s.config.devices![0]!.name).toBe('Living room TV');
    expect(s.canUndo).toBe(false);
  });
  it('previews the in-memory config right after an edit, without waiting for the save', async () => {
    const f = fakeApi({
      'GET api/v1/config': () => ({ body: { hub: 'h', draft: { config: tvConfig(), updated: 1 }, ...META } }),
      'POST api/v1/config/preview': () => ({ body: previewOk() }),
      'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 2, ...META } }),
    });
    const s = new Store(f.api, 'h', 5000);
    await s.load();
    const before = f.calls.filter((c) => c.url.includes('preview')).length;
    s.edit((c) => (c.keys = {}));
    await new Promise((r) => setTimeout(r, 400));
    const live = f.calls.filter((c) => c.url.includes('preview')).slice(before);
    expect(live).toHaveLength(1);
    expect(JSON.parse(live[0]!.body as string).config.keys).toEqual({});
    expect(f.calls.filter((c) => c.method === 'PUT')).toHaveLength(0); // the save is still debounced
  });
});
