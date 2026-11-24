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
import { widgetRect } from '../src/lib/page-renderer';
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

// a 24-button fixed page whose last widget sits top-left: screen 1 draws B23 + B0..B10, screen 2 draws B11..B22
function twoScreens(store: Store) {
  const widgets = Array.from({ length: 24 }, (_, i) => ({ type: 'button', id: `b${i}`, x: ((i + 1) % 3) * 4, y: Math.floor((i + 1) / 3), w: 4, h: 1, label: `B${i}`, action: { none: true } }));
  const wpage = (srcs: number[]) => ({ ...(page as object), version: 2, cols: 12, rows: 4, fixed: true, widgets: srcs.map((src, k) => ({ kind: 'button', x: (k % 3) * 4, y: Math.floor(k / 3), w: 4, h: 1, label: `B${src}`, shape: 'card', icon: 0, icon_only: false, src })) });
  const s1 = wpage([23, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
  const s2 = wpage([11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]);
  store.config.pages = [{ id: 'main', title: 'Main', layout: 'fixed', cols: 12, rows: 9, widgets }] as never;
  store.pages = [{ index: 0, id: 'main', ok: true, error: null, page: s1, wire_pages: [{ page_id: 0, seq: null, bytes: 1, bytes_v1: 1, widgets: 12, page: s1 }, { page_id: 1, seq: null, bytes: 1, bytes_v1: 1, widgets: 12, page: s2 }], bytes: 1, bytes_v1: 1, limit: 251, widgets: 24, widget_limit: 32 }] as never;
  store.dispatchEvent(new Event('change'));
  return { s1, s2 };
}

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

  it('the Red chip carries a swatch in the remote key colour, not an icon', async () => {
    const { palette, store } = await boot();
    (store.config.devices![0]!.commands as Record<string, unknown>)['Red'] = { proto: 'nec', addr: 4, cmd: 77 };
    store.dispatchEvent(new Event('change'));
    await tick(palette);
    const chip = [...palette.shadowRoot.querySelectorAll('.chip2')].find((c) => c.textContent?.trim() === 'Red')!;
    expect(chip.querySelector('svg')).toBeNull();
    const sw = chip.querySelector<HTMLElement>('i.sw')!;
    expect(sw.style.background).toBe('#e0584c');
  });

  describe('multi-screen pages with a reordered compile', () => {
    type Stu = El & { activateWidget(p: number, i: number): void; showCheck(p: number, i: number | undefined): void };
    const wireOf = (remote: El) => (remote as unknown as { st: { wire: number } }).st.wire;
    it('selecting a widget (Show or a click) switches to the screen that draws it', async () => {
      const { remote, studio, store } = await boot();
      twoScreens(store);
      await tick(remote);
      expect(wireOf(remote)).toBe(0);
      (studio as Stu).activateWidget(0, 16);
      await tick(remote);
      expect(wireOf(remote)).toBe(1);
      (studio as Stu).activateWidget(0, 23);
      await tick(remote);
      expect(wireOf(remote)).toBe(0);
      (studio as unknown as { showCheck(p: number, i: number): void }).showCheck(0, 20);
      await tick(remote);
      expect(wireOf(remote)).toBe(1);
      expect(remote.shadowRoot.querySelector('[data-widget="20"]')).toBeTruthy();
    });
    it('offers no empty-cell spots on any screen, and rects exist only for the widgets drawn', async () => {
      const { remote, store } = await boot();
      twoScreens(store);
      (remote as unknown as { st: { wire: number } }).st.wire = 1;
      await tick(remote);
      const spots = (remote as unknown as { spots(): { id: string }[] }).spots();
      expect(spots.filter((x) => x.id.startsWith('c:'))).toEqual([]);
      expect(spots.filter((x) => x.id.startsWith('w:')).length).toBe(12);
      expect(remote.shadowRoot.querySelector('[data-widget="23"]')).toBeNull();
      expect(remote.shadowRoot.querySelector('[data-widget="11"]')).toBeTruthy();
    });
    it('a drop on screen 2 hits the compiled widget under the point, and nothing in an empty area', async () => {
      const { remote, store } = await boot();
      const { s2 } = twoScreens(store);
      (remote as unknown as { st: { wire: number } }).st.wire = 1;
      await tick(remote);
      const r = widgetRect(s2 as never, s2.widgets[5] as never); // compiled index 5 is config widget 16
      const t = remote as unknown as { screenPoint: unknown; renderRoot: { elementsFromPoint: unknown }; targetAt(x: number, y: number): unknown };
      let pt: { x: number; y: number } | null = { x: (r.x1 + r.x2) / 2, y: (r.y1 + r.y2) / 2 };
      t.screenPoint = () => pt;
      t.renderRoot.elementsFromPoint = () => [];
      expect(t.targetAt(0, 0)).toEqual({ kind: 'widget', page: 0, index: 16 });
      pt = { x: 2, y: 310 };
      expect(t.targetAt(0, 0)).toBeNull();
    });
  });

  describe('issue markers and the inspector line', () => {
    const msg = 'Main: "Go" is 4 px too wide for its b13 slot; use "Power"';
    const check = (extra: object = {}) => ({ kind: 'label' as const, page: 0, index: 0, msg, short: 'Power', ...extra });
    // the open Studio republishes its own checks on every render; pin the store's so the fixture stays
    const inject = (store: Store, checks: object[]) => {
      store.setChecks = () => {};
      store.checks = checks as Store['checks'];
      store.dispatchEvent(new Event('change'));
    };
    it('a checked widget gets a circle.mark.warn with the issue text as its title', async () => {
      const { remote, store } = await boot();
      inject(store, [check()]);
      await tick(remote);
      const m = remote.shadowRoot.querySelectorAll('circle.mark');
      expect(m).toHaveLength(1);
      expect(m[0]!.classList.contains('warn')).toBe(true);
      expect(m[0]!.querySelector('title')?.textContent).toBe(msg);
      const r = remote.shadowRoot.querySelector('[data-widget="0"]')!;
      expect(Number(m[0]!.getAttribute('cx'))).toBeGreaterThan(Number(r.getAttribute('x')) + Number(r.getAttribute('width')) - 10);
      expect(Number(m[0]!.getAttribute('cy'))).toBeLessThan(Number(r.getAttribute('y')) + 10);
    });
    it('on a multi-screen fixed page the marker and rect sit at the compiled position on the shown screen only', async () => {
      const { remote, store } = await boot();
      const lab = (i: number) => (i === 15 ? 'Power Toggle' : `B${i}`);
      const cfgPage = { id: 'main', title: 'Main', layout: 'fixed', cols: 12, rows: 7, widgets: Array.from({ length: 20 }, (_, i) => ({ type: 'button', id: `b${i}`, x: (i % 3) * 4, y: Math.floor(i / 3), w: 4, h: 1, label: lab(i), action: { none: true } })) };
      const wpage = (n: number, rows: number, off: number) => ({ ...(page as object), version: 2, cols: 12, rows, fixed: true, widgets: Array.from({ length: n }, (_, k) => ({ kind: 'button', x: (k % 3) * 4, y: Math.floor(k / 3), w: 4, h: 1, label: off + k === 15 ? 'Power' : `B${off + k}`, shape: 'card', icon: 0, icon_only: false })) });
      const s1 = wpage(15, 5, 0);
      const s2 = wpage(5, 2, 15);
      store.config.pages = [cfgPage] as never;
      store.pages = [{ index: 0, id: 'main', ok: true, error: null, page: s1, wire_pages: [{ page_id: 0, seq: null, bytes: 1, bytes_v1: 1, widgets: 15, page: s1 }, { page_id: 1, seq: null, bytes: 1, bytes_v1: 1, widgets: 5, page: s2 }], bytes: 1, bytes_v1: 1, limit: 251, widgets: 20, widget_limit: 32 }] as never;
      inject(store, [{ kind: 'label', page: 0, index: 15, msg: 'x' }, { kind: 'label', page: 0, index: 2, msg: 'y' }]);
      (remote as unknown as { st: { wire: number } }).st.wire = 1;
      await tick(remote);
      const m = remote.shadowRoot.querySelectorAll('circle.mark');
      expect(m).toHaveLength(1); // widget 2 is on screen 1
      const r = widgetRect(s2 as never, s2.widgets[0] as never);
      const rect = remote.shadowRoot.querySelector('[data-widget="15"]')!;
      expect(Number(rect.getAttribute('x'))).toBe(r.x1);
      expect(Number(rect.getAttribute('y'))).toBe(r.y1);
      expect(Number(m[0]!.getAttribute('cx'))).toBe(r.x2 - 3);
      expect(Number(m[0]!.getAttribute('cy'))).toBe(r.y1 + 4);
    });
    it('on screen 2 only screen-2 widgets have rects, at their compiled position, and pressing one selects it', async () => {
      const { remote, store } = await boot();
      const cfgPage = { id: 'main', title: 'Main', layout: 'fixed', cols: 12, rows: 7, widgets: Array.from({ length: 20 }, (_, i) => ({ type: 'button', id: `b${i}`, x: (i % 3) * 4, y: Math.floor(i / 3), w: 4, h: 1, label: `B${i}`, action: { none: true } })) };
      const wpage = (n: number, rows: number, off: number) => ({ ...(page as object), version: 2, cols: 12, rows, fixed: true, widgets: Array.from({ length: n }, (_, k) => ({ kind: 'button', x: (k % 3) * 4, y: Math.floor(k / 3), w: 4, h: 1, label: `B${off + k}`, shape: 'card', icon: 0, icon_only: false })) });
      const s1 = wpage(15, 5, 0);
      const s2 = wpage(5, 2, 15);
      store.config.pages = [cfgPage] as never;
      store.pages = [{ index: 0, id: 'main', ok: true, error: null, page: s1, wire_pages: [{ page_id: 0, seq: null, bytes: 1, bytes_v1: 1, widgets: 15, page: s1 }, { page_id: 1, seq: null, bytes: 1, bytes_v1: 1, widgets: 5, page: s2 }], bytes: 1, bytes_v1: 1, limit: 251, widgets: 20, widget_limit: 32 }] as never;
      (remote as unknown as { st: { wire: number } }).st.wire = 1;
      store.dispatchEvent(new Event('change'));
      await tick(remote);
      const rects = [...remote.shadowRoot.querySelectorAll('[data-widget]')].map((e) => Number(e.getAttribute('data-widget')));
      expect(rects).toEqual([15, 16, 17, 18, 19]);
      const r = widgetRect(s2 as never, s2.widgets[1] as never);
      const el = remote.shadowRoot.querySelector('[data-widget="16"]')!;
      expect([Number(el.getAttribute('x')), Number(el.getAttribute('y'))]).toEqual([r.x1, r.y1]);
      el.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0 }));
      await tick(remote);
      expect((remote as unknown as { st: { sel: unknown } }).st.sel).toMatchObject({ kind: 'widget', page: 0, index: 16 });
    });
    it('a widget with a marker says it has a warning in its label', async () => {
      const { remote, store } = await boot();
      inject(store, [check()]);
      await tick(remote);
      expect(remote.shadowRoot.querySelector('[data-widget="0"]')!.getAttribute('aria-label')).toContain(', has a warning');
    });
    it('the inspector shows one issue line with a fix button, and the page view has no issue list', async () => {
      const { insp, store } = await inspectorFor({ kind: 'widget', page: 0, index: 0 });
      inject(store, [check({ msg: 'x'.repeat(200) + ' use "Power"' })]);
      await tick(insp);
      const lines = insp.shadowRoot.querySelectorAll('.issue');
      expect(lines).toHaveLength(1);
      expect(lines[0]!.textContent).toContain('x'.repeat(200));
      expect(insp.shadowRoot.querySelector('.pxm [role=alert]')).toBeNull();
      expect(insp.shadowRoot.querySelector('.pxm .chips')).toBeNull();
      const cs = getComputedStyle(lines[0]!);
      expect(cs.whiteSpace).not.toBe('nowrap');
      expect(cs.textOverflow).not.toBe('ellipsis');
      await press(insp, 'Use "Power"');
      expect(store.config.pages?.[0]?.widgets[0]?.label).toBe('Power');
    });
    it('a row with no action shows its issue line in the inspector (not tied to the label meter)', async () => {
      const { insp, store } = await inspectorFor({ kind: 'widget', page: 0, index: 0 });
      store.edit((c) => void (c.pages![0]!.widgets[0] = { type: 'button', shape: 'row', id: 'r', x: 0, y: 0, w: 4, h: 1, label: 'Dead' }));
      await tick(insp);
      const lines = insp.shadowRoot.querySelectorAll('.issue');
      expect(lines).toHaveLength(1);
      expect(lines[0]!.textContent).toContain('does nothing');
      expect(lines[0]!.querySelector('button')).toBeNull();
    });
    it('the page view lists no issues', async () => {
      const { insp, store } = await inspectorFor({ kind: 'page', page: 0 });
      inject(store, [check()]);
      await tick(insp);
      expect(insp.shadowRoot.querySelector('.issues, .issue')).toBeNull();
    });
  });
});

describe('studio checks in the store', () => {
  it('publishes checks, shows one on request, and has no floating boxes', async () => {
    const { studio, store } = await boot();
    store.edit((c) => void c.pages![0]!.widgets.push({ type: 'button', shape: 'row', id: 'r', x: 0, y: 1, w: 4, h: 1, label: 'Dead' }));
    await tick(studio);
    expect(store.checks).toHaveLength(1);
    expect(studio.shadowRoot.querySelector('.warnbar, .errbar')).toBeNull();
    const st = (studio as unknown as { st: { sel: unknown } }).st;
    store.showIssue({ level: 'warning', text: 'x', target: { kind: 'widget', page: 0, index: 1 } });
    await tick(studio);
    expect(st.sel).toEqual({ kind: 'widget', page: 0, index: 1 });
    expect(location.hash).toBe('#/remote');
    store.showIssue({ level: 'warning', text: 'x', target: { kind: 'widget', page: 9, index: 0 } });
    expect(st.sel).toEqual({ kind: 'widget', page: 0, index: 1 });
  });
  it('Show on an error with a key path selects that key', async () => {
    const { studio, store } = await boot();
    const st = (studio as unknown as { st: { sel: unknown } }).st;
    store.showIssue({ level: 'error', text: 'x', target: { kind: 'error', path: ['keys', 'menu'] } });
    await tick(studio);
    expect(st.sel).toEqual({ kind: 'key', name: 'menu' });
  });
});

/** The focused element, looking through every shadow root. */
const deepActive = (): Element | null => {
  let a: Element | null = document.activeElement;
  while (a?.shadowRoot?.activeElement) a = a.shadowRoot.activeElement;
  return a;
};
const addDeadRow = async (store: Store, studio: El) => {
  store.edit((c) => void c.pages![0]!.widgets.push({ type: 'button', shape: 'row', id: 'r', x: 0, y: 1, w: 4, h: 1, label: 'Dead' }));
  await tick(studio);
};

describe('focus after Show', () => {
  it('lands on the widget rect when the Studio is mounted', async () => {
    const { studio, store } = await boot();
    await addDeadRow(store, studio);
    store.showIssue({ level: 'warning', text: 'x', target: { kind: 'widget', page: 0, index: 1 } });
    await tick(studio);
    const a = deepActive();
    expect(a?.getAttribute('data-widget')).toBe('1');
    expect(a?.tagName.toLowerCase()).toBe('rect');
  });
  it('lands on the widget rect when the Studio mounts after Show', async () => {
    const { studio, store } = await boot();
    await addDeadRow(store, studio);
    studio.remove();
    store.showIssue({ level: 'warning', text: 'x', target: { kind: 'widget', page: 0, index: 1 } });
    const again = document.createElement('hh-studio') as El;
    document.body.append(again);
    await tick(again);
    const a = deepActive();
    expect(a?.getAttribute('data-widget')).toBe('1');
    expect(a?.tagName.toLowerCase()).toBe('rect');
  });
  it('an error on a key focuses no widget and falls back to the main region', async () => {
    const { app, studio, store } = await boot();
    store.showIssue({ level: 'error', text: 'x', target: { kind: 'error', path: ['keys', 'menu'] } });
    await tick(studio);
    const a = deepActive();
    expect(a?.hasAttribute('data-widget')).toBe(false);
    expect(a).toBe(app.shadowRoot.querySelector('main'));
  });
});

describe('Show while the Studio is not mounted', () => {
  it('keeps the issue pending and selects the widget once the Studio mounts', async () => {
    const { studio, store } = await boot();
    store.edit((c) => void c.pages![0]!.widgets.push({ type: 'button', shape: 'row', id: 'r', x: 0, y: 1, w: 4, h: 1, label: 'Dead' }));
    await tick(studio);
    studio.remove();
    store.showIssue({ level: 'warning', text: 'x', target: { kind: 'widget', page: 0, index: 1 } });
    const again = document.createElement('hh-studio') as El;
    document.body.append(again);
    await tick(again);
    expect((again as unknown as { st: { sel: unknown } }).st.sel).toEqual({ kind: 'widget', page: 0, index: 1 });
    expect(store.pendingIssue).toBeNull();
  });
});
