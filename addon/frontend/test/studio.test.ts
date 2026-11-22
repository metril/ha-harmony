import '../src/components/hh-colour-field'; // lazy in the app; every lazy studio part is loaded up front here (happy-dom cannot upgrade late)
import '../src/components/hh-palette';
import '../src/components/hh-inspector';
import '../src/components/hh-page-strip';
import '../src/components/hh-action-picker';
import '../src/components/hh-ha-browser';
import { beforeEach, describe, expect, it } from 'vitest';
import '../src/components/hh-app';
import keymap from './fixtures/keymap.json';
import vectors from '../../../tests/vectors/pages.json';
import { Store, setStore } from '../src/lib/store';
import { libRoutes, fakeApi, previewOk, tvConfig } from './helpers';

const page = (vectors.valid_v2 as { name: string; dict: unknown }[]).find((v) => v.name === 'v2_all_options')!.dict;
type El = HTMLElement & { updateComplete: Promise<unknown>; shadowRoot: ShadowRoot };

async function boot(ha: 'ok' | 'down' = 'ok', caps: { ha_actions?: boolean } = { ha_actions: true }) {
  const cfg = tvConfig();
  cfg.pages = [{ id: 'main', cols: 4, rows: 4, title: 'Main', widgets: [{ type: 'button', id: 'b', x: 0, y: 0, w: 2, h: 1, label: 'Go', action: { none: true } }] }];
  const f = fakeApi({
    ...libRoutes({ config: cfg }),
    'POST api/v1/config/preview': () => ({ body: previewOk({ pages: [{ index: 0, id: 'main', ok: true, error: null, page, wire_pages: [{ page_id: 0, seq: null, bytes: 40, bytes_v1: 30, widgets: 1, page }], bytes: 40, bytes_v1: 30, limit: 251, widgets: 1, widget_limit: 32 }] }) }),
    'POST api/v1/config/keymap': () => ({ body: keymap }),
    'GET api/v1/hubs': () => ({ body: { hubs: [{ id: 'h', has_draft: true, has_applied: true, applied_rev: 'a', applied_at: 1 }] } }),
    'GET api/v1/health': () => ({ body: { ok: true, addon_version: '1', capabilities: caps } }),
    'GET api/v1/ha/entities': () => (ha === 'ok' ? { body: { entities: [{ entity_id: 'light.lamp', name: 'Lamp', domain: 'light', state: 'on' }, { entity_id: 'scene.movie', name: 'Movie', domain: 'scene' }] } } : { status: 503, body: { error: { code: 'ha_unavailable', msg: 'x', reason: 'status_503' } } }),
    'GET api/v1/ha/services': () => ({ body: { services: [{ domain: 'light', service: 'toggle', name: 'Toggle', fields: [] }, { domain: 'light', service: 'turn_on', name: 'Turn on', fields: ['brightness_pct'] }] } }),
  });
  const store = new Store(f.api, 'h', 'c', 600);
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
    expect(remote.shadowRoot.querySelector('[data-key="menu"]')?.getAttribute('aria-label')).toMatch(/Menu, Default Layer: tv/);
    expect(remote.shadowRoot.querySelector('svg.scr text')?.textContent).toBeTruthy(); // the page renders
    expect(remote.shadowRoot.querySelectorAll('.wh')).toHaveLength(1); // one widget overlay for the config page
  });

  it('click-then-click assigns a palette command to a hard key and announces it; undo reverts', async () => {
    const { remote, palette, studio, store } = await boot();
    await tick(palette, remote);
    const chip = [...palette.shadowRoot.querySelectorAll('.chip2')].find((c) => c.textContent?.trim() === 'Cmd 5') as HTMLElement;
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
    const chip = [...palette.shadowRoot.querySelectorAll('.chip2')].find((c) => c.textContent?.trim() === 'Cmd 7') as HTMLElement;
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

  it('Home Assistant tab lists entities, collapsed by type; a 503 says why and falls back to typing an entity id', async () => {
    const a = await boot('ok');
    a.palette.shadowRoot.querySelectorAll<HTMLElement>('[role="tab"]')[2]!.click();
    await tick(a.palette);
    await tick(a.palette);
    const br = a.palette.shadowRoot.querySelector('hh-ha-browser') as El;
    await br.updateComplete;
    expect(a.palette.shadowRoot.querySelector('[role="tab"][aria-selected="true"]')?.getAttribute('aria-label')).toBe('Home Assistant');
    expect(br.shadowRoot.textContent).toContain('Lights'); // groups start collapsed: no rows yet
    expect(br.shadowRoot.querySelectorAll('.ent').length).toBe(0);
    br.shadowRoot.querySelector<HTMLElement>('.gh')!.click();
    await br.updateComplete;
    expect(br.shadowRoot.textContent).toContain('Lamp');
    document.body.replaceChildren();
    const b = await boot('down');
    b.palette.shadowRoot.querySelectorAll<HTMLElement>('[role="tab"]')[2]!.click();
    await tick(b.palette);
    await tick(b.palette);
    const bb = b.palette.shadowRoot.querySelector('hh-ha-browser') as El;
    await bb.updateComplete;
    expect(bb.shadowRoot.textContent).toContain('cannot be read');
    expect(bb.shadowRoot.textContent).toContain('Retry');
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

  async function inspectorFor(sel: unknown, caps?: { ha_actions?: boolean }) {
    const b = await boot('ok', caps);
    b.store.edit((c) => void c.pages![0]!.widgets.push({ type: 'tile', id: 'lamp', x: 0, y: 1, w: 2, h: 1, label: 'Lamp', entity: 'light.lamp' }));
    await tick(b.remote);
    (b.studio as unknown as { state0: { set: (p: object) => void } }).state0.set({ sel });
    const insp = b.studio.shadowRoot.querySelector('hh-inspector') as El;
    await tick(insp);
    return { ...b, insp };
  }
  const press = async (insp: El, label: string) => {
    ([...insp.shadowRoot.querySelectorAll('button')].find((x) => x.textContent?.trim() === label) as HTMLButtonElement).click();
    await tick(insp);
  };

  it('Align: a three-way icon toggle; Center is the default and clears the key', async () => {
    const { insp, store } = await inspectorFor({ kind: 'widget', page: 0, index: 0 });
    store.edit((c) => void (c.pages![0]!.widgets[0]!.shape = 'row'));
    await tick(insp);
    const btn = (n: string) => insp.shadowRoot.querySelector(`.ali button[aria-label="${n}"]`) as HTMLButtonElement;
    expect(['Align Left', 'Align Center', 'Align Right'].map((n) => btn(n).getAttribute('aria-pressed'))).toEqual(['false', 'true', 'false']);
    btn('Align Right').click();
    await tick(insp);
    expect(store.config.pages![0]!.widgets[0]!.align).toBe('right');
    expect(btn('Align Right').getAttribute('aria-pressed')).toBe('true');
    btn('Align Left').click();
    await tick(insp);
    expect(store.config.pages![0]!.widgets[0]!.align).toBe('left');
    btn('Align Center').click();
    await tick(insp);
    expect('align' in store.config.pages![0]!.widgets[0]!).toBe(false);
  });

  it('Align is disabled with a reason where it has no effect (tiles, small buttons)', async () => {
    const t = await inspectorFor({ kind: 'widget', page: 0, index: 1 });
    expect(t.insp.shadowRoot.querySelector<HTMLButtonElement>('.ali button')!.disabled).toBe(true);
    expect(t.insp.shadowRoot.textContent).toContain('always centred');
  });

  it('tile "When Tapped": Automatic by default and says what runs; Toggle, Action and Nothing write the tap schema', async () => {
    const { insp, store } = await inspectorFor({ kind: 'widget', page: 0, index: 1 });
    expect(insp.shadowRoot.textContent).toContain('Automatic: a tap runs light.toggle on light.lamp.');
    await press(insp, 'Nothing');
    expect(store.config.pages![0]!.widgets[1]!['tap']).toBe('none');
    await press(insp, 'Toggle');
    expect(store.config.pages![0]!.widgets[1]!['tap']).toBe('toggle');
    await press(insp, 'Action');
    expect(store.config.pages![0]!.widgets[1]!['tap']).toEqual({ ha: { service: 'light.toggle', target: { entity_id: 'light.lamp' } } });
    await press(insp, 'Automatic');
    expect('tap' in store.config.pages![0]!.widgets[1]!).toBe(false);
  });

  it('tile "When Tapped" is disabled with a reason when the add-on cannot store taps', async () => {
    const { insp } = await inspectorFor({ kind: 'widget', page: 0, index: 1 }, { ha_actions: false });
    expect(insp.shadowRoot.textContent).toContain('Disabled: this add-on version cannot store tile taps yet');
    expect([...insp.shadowRoot.querySelectorAll('button')].some((x) => x.textContent?.trim() === 'Toggle')).toBe(false);
  });

  it('Home Assistant action picker: service and entity from the add-on, a data field, repeat while held -> the ha action', async () => {
    const { insp, store } = await inspectorFor({ kind: 'key', name: 'red' });
    const picker = insp.shadowRoot.querySelector('hh-action-picker') as El;
    await tick(picker);
    const root = picker.shadowRoot;
    ([...root.querySelectorAll('.seg button')].find((x) => x.textContent === 'Home Assistant Action') as HTMLButtonElement).click();
    await tick(picker);
    const svc = root.querySelector('select') as HTMLSelectElement;
    expect([...svc.options].map((o) => o.value)).toEqual(expect.arrayContaining(['light.toggle', 'light.turn_on']));
    svc.value = 'light.turn_on';
    svc.dispatchEvent(new Event('change'));
    const ent = root.querySelector('input[placeholder="Search entities"]') as HTMLInputElement;
    ent.value = 'light.lamp';
    ent.dispatchEvent(new Event('input'));
    await tick(picker);
    const field = root.querySelector('input[aria-label="Field brightness_pct"]') as HTMLInputElement;
    field.value = '50';
    field.dispatchEvent(new Event('input'));
    const rep = root.querySelector('.chk input') as HTMLInputElement;
    rep.checked = true;
    rep.dispatchEvent(new Event('change'));
    await tick(picker);
    ([...root.querySelectorAll('button')].find((x) => x.textContent === 'Set Action') as HTMLButtonElement).click();
    await tick(insp);
    expect(store.config.keys!['red']).toEqual({ ha: { service: 'light.turn_on', target: { entity_id: 'light.lamp' }, data: { brightness_pct: 50 }, repeat: true } });
  });

  it('Home Assistant action picker is disabled with a reason when the add-on cannot store it', async () => {
    const { insp } = await inspectorFor({ kind: 'key', name: 'red' }, { ha_actions: false });
    const picker = insp.shadowRoot.querySelector('hh-action-picker') as El;
    await tick(picker);
    ([...picker.shadowRoot.querySelectorAll('.seg button')].find((x) => x.textContent === 'Home Assistant Action') as HTMLButtonElement).click();
    await tick(picker);
    expect(picker.shadowRoot.textContent).toContain('this add-on version has no Home Assistant actions');
    expect((([...picker.shadowRoot.querySelectorAll('button')].find((x) => x.textContent === 'Set Action')) as HTMLButtonElement).disabled).toBe(true);
  });

  it('a key on a Home Assistant action shows an HA badge, not Send IR, and reads "Lamp · toggle"', async () => {
    const { insp, store } = await inspectorFor({ kind: 'key', name: 'red' });
    store.edit((c) => void (c.keys = { ...c.keys, red: { ha: { service: 'light.toggle', target: { entity_id: 'light.lamp' }, repeat: true } } }));
    const picker = insp.shadowRoot.querySelector('hh-action-picker') as El;
    await tick(picker);
    await tick(picker);
    const send = picker.shadowRoot.querySelector('hh-send-ir') as El;
    await send.updateComplete;
    expect(send.shadowRoot.querySelector('button')).toBeNull();
    expect(send.shadowRoot.querySelector('.ha')!.textContent).toBe('HA');
    expect(picker.shadowRoot.querySelector('.cur b')!.textContent).toMatch(/^.+ \u00B7 toggle \u21BB$/);
  });

  it('the key panel offers Send IR for the code the key sends, and palette chips carry a function icon', async () => {
    const { insp, palette } = await inspectorFor({ kind: 'key', name: 'volume_up' });
    const picker = insp.shadowRoot.querySelector('hh-action-picker') as El;
    await tick(picker);
    const send = picker.shadowRoot.querySelector('hh-send-ir') as El;
    expect(send).toBeTruthy();
    await send.updateComplete;
    expect(send.shadowRoot.querySelector('button')!.getAttribute('aria-label')).toBe('Send IR for Living room TV · Cmd 1');
    await tick(palette);
    expect(palette.shadowRoot.querySelectorAll('.chip2 svg.sic').length).toBeGreaterThan(5);
  });
});
