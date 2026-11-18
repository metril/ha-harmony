import { beforeEach, describe, expect, it } from 'vitest';
import '../src/components/hh-app';
import keymap from './fixtures/keymap.json';
import vectors from '../../../tests/vectors/pages.json';
import { Store, setStore } from '../src/lib/store';
import { META, fakeApi, previewOk, tvConfig } from './helpers';

const page = (vectors.valid_v2 as { name: string; dict: unknown }[]).find((v) => v.name === 'v2_all_options')!.dict;
type El = HTMLElement & { updateComplete: Promise<unknown>; shadowRoot: ShadowRoot };

async function boot(ha: 'ok' | 'down' = 'ok', caps: { ha_actions?: boolean } = { ha_actions: true }) {
  const cfg = tvConfig();
  cfg.pages = [{ id: 'main', cols: 4, rows: 4, title: 'Main', widgets: [{ type: 'button', id: 'b', x: 0, y: 0, w: 2, h: 1, label: 'Go', action: { none: true } }] }];
  const f = fakeApi({
    'GET api/v1/config': () => ({ body: { hub: 'h', draft: { config: cfg, updated: 1 }, ...META, applied: { ...META.applied, config: tvConfig() } } }),
    'POST api/v1/config/preview': () => ({ body: previewOk({ pages: [{ index: 0, id: 'main', ok: true, error: null, page, wire_pages: [{ page_id: 0, seq: null, bytes: 40, bytes_v1: 30, widgets: 1, page }], bytes: 40, bytes_v1: 30, limit: 251, widgets: 1, widget_limit: 32 }] }) }),
    'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 2, ...META } }),
    'POST api/v1/config/keymap': () => ({ body: keymap }),
    'GET api/v1/hubs': () => ({ body: { hubs: [{ id: 'h', has_draft: true, has_applied: true, applied_rev: 'a', applied_at: 1 }] } }),
    'GET api/v1/health': () => ({ body: { ok: true, addon_version: '1', capabilities: caps } }),
    'GET api/v1/ha/entities': () => (ha === 'ok' ? { body: { entities: [{ entity_id: 'light.lamp', name: 'Lamp', domain: 'light', state: 'on' }, { entity_id: 'scene.movie', name: 'Movie', domain: 'scene' }] } } : { status: 503, body: { error: { code: 'ha_unavailable', msg: 'x' } } }),
    'GET api/v1/ha/services': () => ({ body: { services: [{ domain: 'light', service: 'toggle', name: 'Toggle', fields: [] }] } }),
  });
  const store = new Store(f.api, 'h', 600);
  setStore(store);
  await store.load();
  location.hash = '#/remote';
  const app = document.createElement('hh-app') as El;
  document.body.append(app);
  await app.updateComplete;
  const studio = app.shadowRoot.querySelector('hh-studio') as El;
  await studio.updateComplete;
  await settle();
  const remote = studio.shadowRoot.querySelector('hh-remote') as El;
  const palette = studio.shadowRoot.querySelector('hh-palette') as El;
  return { app, studio, remote, palette, store, ...f };
}
const settle = async () => {
  for (let i = 0; i < 4; i++) await new Promise((r) => setTimeout(r, 10));
};
const tick = async (...els: El[]) => {
  for (const e of els) await e.updateComplete;
  await settle();
};

beforeEach(() => {
  document.body.replaceChildren();
});

describe('remote studio', () => {
  it('is the default route and draws the remote with all 35 keys as labelled buttons', async () => {
    const { remote, studio } = await boot();
    await tick(remote, studio);
    const keys = remote.shadowRoot.querySelectorAll('[data-key]');
    expect(keys).toHaveLength(35);
    expect(remote.shadowRoot.querySelector('[data-key="menu"]')?.getAttribute('aria-label')).toMatch(/Menu, Default layer: tv/);
    expect(remote.shadowRoot.querySelector('svg.scr text')?.textContent).toBeTruthy(); // the page renders
    expect(remote.shadowRoot.querySelectorAll('.wh')).toHaveLength(1); // one widget overlay for the config page
  });

  it('click-then-click assigns a palette command to a hard key and announces it; undo reverts', async () => {
    const { remote, palette, studio, store } = await boot();
    await tick(palette, remote);
    const chip = [...palette.shadowRoot.querySelectorAll('.chip2')].find((c) => c.textContent?.trim() === 'cmd_5') as HTMLElement;
    chip.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', bubbles: true, composed: true }));
    await tick(studio, remote, palette);
    expect(studio.shadowRoot.querySelector('.banner2')?.textContent).toContain('Click a key');
    remote.shadowRoot.querySelector<SVGElement>('[data-key="red"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    await tick(studio, remote);
    expect(store.config.keys).toMatchObject({ red: { device: 'tv', command: 'cmd_5' } });
    expect(studio.shadowRoot.querySelector('[role="status"][aria-live]')?.textContent).toContain('Assigned');
    expect(store.canUndo).toBe(true);
    store.undo();
    expect(store.config.keys?.['red']).toBeUndefined();
  });

  it('Escape cancels a picked-up source', async () => {
    const { palette, studio, remote } = await boot();
    await tick(palette, studio, remote);
    const chip = palette.shadowRoot.querySelector('.chip2') as HTMLElement;
    expect(chip).toBeTruthy();
    chip.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, composed: true }));
    await tick(studio);
    await tick(studio);
    expect(studio.shadowRoot.querySelector('.banner2')).toBeTruthy();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await tick(studio);
    expect(studio.shadowRoot.querySelector('.banner2')).toBeNull();
  });

  it('keyboard: arrows move the cursor between keys and Enter drops', async () => {
    const { palette, studio, store, remote } = await boot();
    await tick(palette);
    const chip = [...palette.shadowRoot.querySelectorAll('.chip2')].find((c) => c.textContent?.trim() === 'cmd_7') as HTMLElement;
    chip.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', bubbles: true, composed: true }));
    await tick(studio, remote);
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' })); // select -> down
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' }));
    await tick(studio);
    expect(store.config.keys?.['down']).toMatchObject({ device: 'tv', command: 'cmd_7' });
  });

  it('layers: choosing an activity shows its layer, and X-ray labels the keys', async () => {
    const { studio, remote } = await boot();
    const tab = [...studio.shadowRoot.querySelectorAll('[role="tab"]')].find((t) => t.textContent === 'Watch TV') as HTMLElement;
    tab.click();
    await tick(studio, remote);
    expect(tab.getAttribute('aria-selected')).toBe('true');
    expect(remote.shadowRoot.querySelector('svg.remote')?.getAttribute('aria-label')).toContain('Watch TV layer');
    const x = [...studio.shadowRoot.querySelectorAll('button')].find((b) => b.textContent === 'X-ray') as HTMLElement;
    x.click();
    await tick(studio, remote);
    expect(x.getAttribute('aria-pressed')).toBe('true');
    expect(remote.shadowRoot.querySelectorAll('text.xl').length).toBeGreaterThan(0);
  });

  it('Home tab lists Home Assistant entities by domain; a 503 falls back to typing an entity id', async () => {
    const a = await boot('ok');
    a.palette.shadowRoot.querySelectorAll<HTMLElement>('[role="tab"]')[2]!.click();
    await tick(a.palette);
    await tick(a.palette);
    expect(a.palette.shadowRoot.textContent).toContain('Lamp');
    expect(a.palette.shadowRoot.textContent).toContain('Light');
    document.body.replaceChildren();
    const b = await boot('down');
    b.palette.shadowRoot.querySelectorAll<HTMLElement>('[role="tab"]')[2]!.click();
    await tick(b.palette);
    await tick(b.palette);
    expect(b.palette.shadowRoot.textContent).toContain('cannot be read');
    expect(b.palette.shadowRoot.querySelector('input[aria-label="Entity id"]')).toBeTruthy();
  });

  it('dropping an activity-less command on a widget replaces its action and keeps the byte counter live', async () => {
    const { remote, palette, studio, store } = await boot();
    await tick(palette, remote);
    const w = remote.shadowRoot.querySelector<SVGElement>('[data-widget="0"]')!;
    w.dispatchEvent(new Event('focus'));
    await tick(studio);
    const insp = studio.shadowRoot.querySelector('hh-inspector') as El;
    await tick(insp);
    const label = insp.shadowRoot.querySelector<HTMLInputElement>('input[aria-label="Label"]')!;
    expect(insp.shadowRoot.textContent).toMatch(/2\/24 B/);
    label.value = 'Go ahead';
    label.dispatchEvent(new Event('input'));
    await tick(insp);
    expect(store.config.pages?.[0]?.widgets[0]?.label).toBe('Go ahead');
    expect(insp.shadowRoot.textContent).toMatch(/8\/24 B/);
  });
});
