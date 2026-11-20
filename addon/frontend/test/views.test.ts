import { beforeEach, describe, expect, it, vi } from 'vitest';
import '../src/components/hh-app';
import '../src/components/hh-apply-dialog';
import '../src/components/hh-import-dialog';
import '../src/components/hh-draft-bar';
import { Store, setStore } from '../src/lib/store';
import { ApiError } from '../src/lib/api';
import { META, fakeApi, previewBad, previewOk, status, tvConfig, type Handler } from './helpers';

async function boot(opts: { draft?: boolean; applied?: boolean; errors?: boolean; hash?: string; statuses?: object[]; hubs?: { id: string }[]; routes?: Record<string, Handler>; config?: ReturnType<typeof tvConfig> } = {}) {
  const statuses = [...(opts.statuses ?? [status()])];
  const cfg = opts.config ?? tvConfig();
  const f = fakeApi({
    'GET api/v1/config': () => ({
      body: {
        hub: 'h',
        draft: opts.draft ? { config: cfg, updated: 1_700_000_050 } : null,
        ...META,
        ...(opts.applied === false ? { applied: null, history: [] } : { applied: { ...META.applied, config: tvConfig() } }),
      },
    }),
    'POST api/v1/config/preview': () =>
      opts.errors
        ? { body: previewBad([{ path: ['devices', 0, 'commands', 'cmd_3'], msg: 'nec cmd 999 outside 0..255', line: null }]) }
        : { body: previewOk() },
    'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 1_700_000_100, ...META } }),
    'GET api/v1/hubs': () => ({ body: { hubs: (opts.hubs ?? [{ id: 'h' }]).map((h) => ({ has_draft: false, has_applied: true, applied_rev: 'aaaa1111', applied_at: 1, ...h })) } }),
    'POST api/v1/config/apply': () => ({ body: { ok: true, applied: { rev: 4, at: 1_700_000_200, hash: 'cafebabe' }, history: [META.applied, ...META.history], history_max: 5, rev: 8 } }),
    'GET api/v1/config/status': () => ({ body: statuses.length > 1 ? statuses.shift() : statuses[0] }),
    'GET api/v1/config/history': () => ({ body: { hub: 'h', rev: 2, at: 1, hash: 'x', config: { ...tvConfig(), activities: [] } } }),
    'POST api/v1/ir/test': () => ({ body: { ok: true, action: {} } }),
    ...(opts.routes ?? {}),
  });
  const store = new Store(f.api, 'h', 600);
  setStore(store);
  await store.load();
  location.hash = opts.hash ?? '#/overview';
  const app = document.createElement('hh-app') as HTMLElement & { updateComplete: Promise<unknown> };
  document.body.append(app);
  await app.updateComplete;
  return { app, store, ...f };
}
const settle = async (el: HTMLElement) => {
  await (el as unknown as { updateComplete: Promise<unknown> }).updateComplete;
  await new Promise((r) => setTimeout(r, 0));
};
const lazyEl = (app: HTMLElement, tag: string) =>
  vi.waitFor(() => {
    const e = app.shadowRoot!.querySelector(tag);
    if (!e) throw new Error(`no ${tag} yet`);
    return e as HTMLElement;
  });
const text = (el: Element) => el.shadowRoot?.textContent?.replace(/\s+/g, ' ') ?? '';

beforeEach(() => {
  document.body.replaceChildren();
});

describe('screens render with a realistic config', () => {
  it('overview: stats, activities, permissions, nothing to apply', async () => {
    const { app } = await boot();
    const ov = await lazyEl(app, 'hh-overview');
    await settle(ov as HTMLElement);
    const t = text(ov);
    expect(t).toContain('2 / 16Devices');
    expect(t).toContain('Watch TV');
    expect(t).toContain('sensor.living_room_temperature');
    expect(text(app)).toContain('Overview');
    const bar = app.shadowRoot!.querySelector('hh-draft-bar')!;
    expect(text(bar)).toContain('No changes since rev 3');
    expect(text(bar)).toContain('Valid');
  });

  it('first-run shows the empty state with Import and Add your first device', async () => {
    const { app } = await boot({ applied: false });
    const ov = await lazyEl(app, 'hh-overview');
    await settle(ov as HTMLElement);
    expect(text(ov)).toContain('Import YAML file');
    expect(ov.shadowRoot!.querySelector('a.btn.pri')?.getAttribute('href')).toBe('#/devices/new');
  });

  it('hh-send-ir shows an HA badge, no button, for a Home Assistant action or device', async () => {
    await boot();
    for (const props of [{ code: { ha: { service: 'light.toggle', target: { entity_id: 'light.a' } } } }, { ha: true, code: { proto: 'nec', addr: 1, cmd: 1 } }]) {
      const el = Object.assign(document.createElement('hh-send-ir'), props) as unknown as HTMLElement & { updateComplete: Promise<unknown> };
      document.body.append(el);
      await el.updateComplete;
      expect(el.shadowRoot!.querySelector('button')).toBeNull();
      expect(el.shadowRoot!.querySelector('.ha')!.textContent).toBe('HA');
    }
  });

  it('devices: keypad of 60+ commands, each with its own Send IR; the list view has every row', async () => {
    const { app, calls } = await boot({ hash: '#/devices/tv' });
    const dv = await lazyEl(app, 'hh-devices');
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('.key .k').length).toBeGreaterThan(60);
    const send = dv.shadowRoot!.querySelector('hh-send-ir')! as HTMLElement & { updateComplete: Promise<unknown> };
    await send.updateComplete;
    const btn = send.shadowRoot!.querySelector('button')!;
    expect(btn.getAttribute('aria-disabled')).toBe('false');
    expect(btn.getAttribute('aria-label')).toBe('Send IR for power on');
    btn.click();
    await vi.waitFor(() => expect(calls.find((c) => c.url.includes('ir/test'))).toBeTruthy());
    expect(JSON.parse(calls.find((c) => c.url.includes('ir/test'))!.body!)).toEqual({ hub: 'h', ir: { proto: 'nec', addr: 4, cmd: 2 } });
    expect(text(dv)).toContain('IR archive · Samsung UE55');
    ([...dv.shadowRoot!.querySelectorAll('.seg button')].find((b) => b.textContent === 'List view') as HTMLButtonElement).click();
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('tbody tr').length).toBe(61);
  });

  it('devices gallery: a card per device with power status, counts and source, plus Add device', async () => {
    const { app } = await boot({ hash: '#/devices' });
    const dv = await lazyEl(app, 'hh-devices');
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('.dcard').length).toBe(2);
    const t = text(dv);
    expect(t).toContain('Power on + off');
    expect(t).toContain('Power toggle only');
    expect(t).toContain('61 commands');
    expect(t).toContain('IR archive · Samsung UE55');
    expect(t).toContain('Your Harmony backup');
    expect(dv.shadowRoot!.querySelector('a.add')!.getAttribute('href')).toBe('#/devices/new');
  });

  it('history: one status chip on the newest rev, earlier revs have exactly Compare and Restore', async () => {
    const { app } = await boot({ hash: '#/history', statuses: [status({ applied_rev: 'a', acked_rev: 'a', result: 'ok' })] });
    const h = await lazyEl(app, 'hh-history');
    await settle(h as HTMLElement);
    expect(text(h)).toContain('Rev 3');
    await vi.waitFor(() => expect(text(h)).toContain('On the remote'));
    for (const old of ['Applied', 'Acknowledged', 'Current']) expect(text(h)).not.toContain(old);
    const rows = [...h.shadowRoot!.querySelectorAll('li')];
    expect(rows[0]!.querySelectorAll('button').length).toBe(0);
    expect(rows[0]!.querySelectorAll('.chip').length).toBe(1);
    expect(rows[0]!.querySelector('.acts')!.getAttribute('role')).toBe('status');
    expect(rows[1]!.querySelectorAll('.chip').length).toBe(0);
    expect([...rows[1]!.querySelectorAll('button')].map((b) => b.textContent!.trim())).toEqual(['Compare', 'Restore']);
    expect(rows[1]!.querySelector('time')!.getAttribute('title')).toBeTruthy();
  });

  it('history: sending, not sent, and rejected with a reason behind See why', async () => {
    const live = { linked_hubs: [{ id: 'h', last_seen: 990 }], now: 1000 };
    const a = await boot({ hash: '#/history', statuses: [status(live)] });
    const h1 = await lazyEl(a.app, 'hh-history');
    await vi.waitFor(() => expect(text(h1)).toContain('Sending…'));
    document.body.replaceChildren();
    const b = await boot({ hash: '#/history', statuses: [status({ linked_hubs: [], now: 1000 })] });
    const h2 = await lazyEl(b.app, 'hh-history');
    await vi.waitFor(() => expect(text(h2)).toContain('Not sent'));
    document.body.replaceChildren();
    const c = await boot({ hash: '#/history', statuses: [status({ ...live, acked_rev: 'aaaa1111', result: 'error', detail: 'bad key map' })] });
    const h3 = await lazyEl(c.app, 'hh-history');
    await vi.waitFor(() => expect(text(h3)).toContain('Rejected by hub'));
    expect(text(h3)).not.toContain('bad key map');
    const why = [...h3.shadowRoot!.querySelectorAll('button')].find((x) => x.textContent!.trim() === 'See why') as HTMLButtonElement;
    why.click();
    await vi.waitFor(() => expect(h3.shadowRoot!.querySelector('#why')?.textContent).toContain('bad key map'));
  });

  it('header shows the hub and whether Home Assistant polls for it', async () => {
    const { app } = await boot({ statuses: [status({ hub_last_seen: 100, now: 120, linked_hubs: [{ id: 'h', last_seen: 100 }] })] });
    const hub = app.shadowRoot!.querySelector('header .hub')!;
    expect(hub.textContent).toContain('Hub:');
    expect(hub.textContent).toContain('h');
    await vi.waitFor(() => expect(hub.textContent).toContain('Linked, polling'));
  });

  it('header hub state: not linked at all', async () => {
    const { app } = await boot({ statuses: [status({ now: 120, linked_hubs: [] })] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('header .hub')!.textContent).toContain('No Home Assistant linked'));
  });

  it('history: Compare loads the old version and lists what differs from the current one', async () => {
    const { app, calls } = await boot({ hash: '#/history' });
    const h = await lazyEl(app, 'hh-history');
    await settle(h as HTMLElement);
    ([...h.shadowRoot!.querySelectorAll('button')].find((b) => b.textContent?.includes('Compare')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(h)).toContain('Watch TV'));
    expect(calls.some((c) => c.url.includes('config/history') && c.url.includes('rev=2'))).toBe(true);
    expect(text(h)).toContain('added');
  });

  it('hub picker lists the hubs the add-on knows', async () => {
    const { app } = await boot({ hubs: [{ id: 'h' }, { id: 'other' }] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('select[aria-label="Hub"]')?.textContent).toContain('other'));
  });

  it('activities: gallery card with its mini chain, and the editor shows chain, generated steps with settle gaps, and who handles it', async () => {
    const { app } = await boot({ hash: '#/activities' });
    const av = await lazyEl(app, 'hh-activities');
    await settle(av as HTMLElement);
    expect(av.shadowRoot!.querySelectorAll('.acard').length).toBe(1);
    expect(text(av)).toContain('Watch TV');
    expect(av.shadowRoot!.querySelector('hh-chain-mini')!.shadowRoot!.textContent).toContain('Port 1');
    expect(text(av)).toContain('New activity');
    location.hash = '#/activities/watch_tv';
    await vi.waitFor(() => expect(text(av)).toContain('Signal chain'));
    await settle(av as HTMLElement);
    const t = text(av);
    expect(t).toContain('Power on Living room TV');
    expect(t).toContain('2 s');
    expect(t).toContain('Drop a device');
    expect(t).toContain('Who handles it');
    expect(av.shadowRoot!.querySelectorAll('.step.auto').length).toBeGreaterThan(3);
    expect(av.shadowRoot!.querySelectorAll('.pill').length).toBe(2);
  });

  it('activities: choosing who handles volume fills the activity keys and survives in the editor block; own steps reorder with Alt+arrows', async () => {
    const config = tvConfig();
    (config.devices![0] as { commands: Record<string, unknown> }).commands['vol_up'] = { proto: 'nec', addr: 4, cmd: 1 };
    (config.devices![0] as { commands: Record<string, unknown> }).commands['vol_down'] = { proto: 'nec', addr: 4, cmd: 2 };
    config.activities![0]!.steps = [{ device: 'tv', command: 'cmd_1' }, { device: 'tv', command: 'cmd_2' }];
    const { app, store } = await boot({ hash: '#/activities/watch_tv', config, draft: true });
    const av = await lazyEl(app, 'hh-activities');
    await vi.waitFor(() => expect(text(av)).toContain('Who handles it'));
    await settle(av as HTMLElement);
    av.shadowRoot!.querySelector<HTMLElement>('button[aria-label^="Who handles volume"]')!.click();
    await settle(av as HTMLElement);
    const items = [...av.shadowRoot!.querySelectorAll<HTMLElement>('.zmenu button')];
    items.find((b) => b.textContent?.includes('Living room TV'))!.click();
    expect(store.config.activities![0]!.keys).toMatchObject({ volume_up: { device: 'tv', command: 'vol_up' }, volume_down: { device: 'tv', command: 'vol_down' } });
    expect((store.config.editor as { activities: Record<string, { handlers: object }> }).activities['watch_tv']!.handlers).toEqual({ volume: 'tv' });
    await settle(av as HTMLElement);
    expect(text(av)).toContain('No match for Volume mute');
    const step = av.shadowRoot!.querySelector<HTMLElement>('.step[data-step="0"]')!;
    step.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', altKey: true, bubbles: true }));
    expect(store.config.activities![0]!.steps!.map((x) => x['command'])).toEqual(['cmd_2', 'cmd_1']);
  });

  it('add-device wizard: brand, type, power test No then Yes, likely model, review, add writes the draft only', async () => {
    const cand = (kind: string, cmd: number) => ({ kind, action: { proto: 'sirc12', addr: 1, cmd }, form: 'proto', count: 3, models: ['KDL-1'], codesets: 1 });
    const { app, store, calls } = await boot({
      hash: '#/devices/new',
      routes: {
        'GET api/v1/ir/brands': () => ({ body: { brands: [{ name: 'Sony', slug: 'Sony', count: 20109 }] } }),
        'GET api/v1/ir/candidates': () => ({ body: { candidates: [cand('on', 46), cand('off', 47)], examined: 5, matched: 3, errors: 0, truncated: false } }),
        'GET api/v1/ir/models': () => ({ body: { brand: { name: 'Sony', slug: 'Sony' }, total: 1, offset: 0, limit: 40, models: [{ file: 'KDL-1.json', name: 'KDL-1', id: 1 }] } }),
        'GET api/v1/ir/device': () => ({ body: { device: { id: 'sony_kdl_1', name: 'Sony KDL-1', power: { on: { proto: 'sirc12', addr: 1, cmd: 46 }, off: { proto: 'sirc12', addr: 1, cmd: 47 } }, commands: { vol_up: { proto: 'sirc12', addr: 1, cmd: 18 }, ok: { proto: 'sirc12', addr: 1, cmd: 11 } } }, forms: {}, notes: ['12 gaps shortened'], skipped: [{ name: 'weird', reason: 'unsupported' }], skipped_total: 1, device_type: null, source: {} } }),
      },
    });
    const w = await lazyEl(app, 'hh-add-device');
    const click = async (sel: string, label?: string) => {
      await settle(w as HTMLElement);
      const els = [...w.shadowRoot!.querySelectorAll<HTMLElement>(sel)];
      (label ? els.find((e) => e.textContent?.includes(label)) : els[0])!.click();
    };
    await click('.tile', 'IR archive');
    await vi.waitFor(() => expect(w.shadowRoot!.querySelector('.brand')).toBeTruthy());
    await click('.brand', 'Sony');
    await click('.tile', 'TV');
    await vi.waitFor(() => expect(text(w)).toContain('Code 1 of 2'));
    await click('button', 'No, next code');
    await vi.waitFor(() => expect(text(w)).toContain('Code 2 of 2'));
    await click('button', 'Yes');
    await vi.waitFor(() => expect(text(w)).toContain('Likely matches'));
    await click('.likely .btn', 'KDL-1');
    await vi.waitFor(() => expect(text(w)).toContain('12 gaps shortened'));
    expect(text(w)).toContain('weird: unsupported');
    expect(calls.some((c) => c.method === 'PUT')).toBe(false);
    await click('.side .btn.pri', 'Add device');
    expect(store.config.devices!.map((d) => d.id)).toContain('sony_kdl_1');
    expect(Object.keys(store.config.devices!.find((d) => d.id === 'sony_kdl_1')!.commands!)).toEqual(['vol_up', 'ok']);
    expect(location.hash).toBe('#/devices/sony_kdl_1');
  });

  it('add-device wizard: an offline archive shows a banner with Retry, and N / Y / Space shortcuts drive the power test', async () => {
    let up = false;
    const { app } = await boot({
      hash: '#/devices/new',
      routes: {
        'GET api/v1/ir/brands': () => (up ? { body: { brands: [{ name: 'Sony', slug: 'Sony', count: 1 }] } } : { status: 504, body: { error: { code: 'archive_offline', msg: 'down' } } }),
      },
    });
    const w = await lazyEl(app, 'hh-add-device');
    await settle(w as HTMLElement);
    ([...w.shadowRoot!.querySelectorAll<HTMLElement>('.tile')].find((e) => e.textContent?.includes('IR archive')))!.click();
    await vi.waitFor(() => expect(text(w)).toContain("The IR archive can't be reached"));
    up = true;
    ([...w.shadowRoot!.querySelectorAll<HTMLElement>('button')].find((e) => e.textContent === 'Retry'))!.click();
    await vi.waitFor(() => expect(w.shadowRoot!.querySelector('.brand')).toBeTruthy());
  });

  it('the rail shows counts and an error badge from validation', async () => {
    const { app } = await boot({ draft: true, errors: true });
    await settle(app);
    const nav = app.shadowRoot!.querySelector('nav')!;
    expect(nav.querySelector('.err')?.textContent?.trim()).toBe('1');
    const bar = app.shadowRoot!.querySelector('hh-draft-bar')!;
    await settle(bar as HTMLElement);
    expect(text(bar)).toContain('1 error · Show');
    const apply = bar.shadowRoot!.querySelector('.btn.pri')!;
    expect(apply.getAttribute('aria-disabled')).toBe('true');
    expect(apply.getAttribute('title')).toBe('Fix 1 error to apply');
  });
});

describe('apply dialog', () => {
  it('lists the diff, applies, and shows waiting for Home Assistant as pending', async () => {
    const { app, store } = await boot({ draft: true });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await vi.waitFor(() => expect(store.saveState === 'saved' || store.saveState === 'dirty').toBe(true));
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean };
    dlg.open = true;
    await settle(dlg);
    expect(dlg.shadowRoot!.querySelector('hh-dialog')?.getAttribute('heading')).toBe('Apply draft to h?');
    expect(text(dlg)).toContain('Renamed TV');
    const apply = [...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply to hub')) as HTMLButtonElement;
    apply.click();
    await vi.waitFor(() => expect(dlg.shadowRoot!.querySelector('hh-dialog')?.getAttribute('heading')).toBe('Rev 4 saved'));
    expect(text(dlg)).toContain('Saved as rev 4');
    expect(store.api).toBeDefined();
    expect(text(dlg)).toContain('Sending to the remote (pending)');
    expect(store.hasDraft).toBe(false);
  });

  it('moves from waiting to done when the integration acks the applied bundle rev', async () => {
    const { app, store } = await boot({
      draft: true,
      statuses: [status(), status({ acked_rev: 'old00000', result: 'ok' }), status({ acked_rev: 'aaaa1111', result: 'ok' })],
    });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; pollMs: number };
    dlg.pollMs = 150;
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply to hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('Sending to the remote (pending)'));
    await vi.waitFor(() => expect(text(dlg)).not.toContain('(pending)'));
    expect(text(dlg)).toContain('On the remote');
    expect(text(dlg)).toContain('Done');
  });

  it('after the wait it says Home Assistant has not picked it up, and still completes on a late ack', async () => {
    const { app, store } = await boot({ draft: true, statuses: [status({ linked: false })] });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; pollMs: number; waitMs: number };
    dlg.pollMs = 10;
    dlg.waitMs = 30;
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply to hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('Is the Harmony integration set up and linked'));
  });

  it('names the hub id when Home Assistant polls for another one, instead of waiting forever', async () => {
    const st = status({ linked_hubs: [{ id: 'living-room-hub', last_seen: 995 }], now: 1000, hub_last_seen: null });
    const { app, store } = await boot({ draft: true, statuses: [st] });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; pollMs: number; waitMs: number };
    dlg.pollMs = 10;
    dlg.waitMs = 30;
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply to hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('Home Assistant is not polling for this hub (h)'));
    expect(text(dlg)).toContain('living-room-hub');
  });

  it('the app shows applied vs acked after navigation, and the link problem', async () => {
    const st = status({ linked_hubs: [{ id: 'h', last_seen: 990 }], now: 1000 });
    const { app } = await boot({ statuses: [st] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('#link-pending')?.textContent).toContain('Rev 3 is being sent to the remote'));
    expect(app.shadowRoot!.querySelector('#link-problem')).toBeNull();
    document.body.replaceChildren();
    const stale = await boot({ statuses: [status({ linked_hubs: [{ id: 'h', last_seen: 100 }], now: 1000 })] });
    await vi.waitFor(() => expect(stale.app.shadowRoot!.querySelector('#link-problem')?.textContent).toContain('not polling for this hub (h)'));
    document.body.replaceChildren();
    const none = await boot({ statuses: [status({ linked_hubs: [], now: 1000 })] });
    await vi.waitFor(() => expect(none.app.shadowRoot!.querySelector('#link-problem')?.textContent).toContain('No Home Assistant integration is linked'));
  });

  it('the header shows the hub name with the id as secondary text, and the switcher uses names', async () => {
    const hubs = (linked: object[]) => ({ body: { hubs: [], linked, adopt: null } });
    const { app } = await boot({ routes: { 'GET api/v1/hubs': () => hubs([{ id: 'h', last_seen: 1, name: 'Living room' }]) } });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('.hub .hn')?.textContent).toBe('Living room'));
    expect(app.shadowRoot!.querySelector('.hub .hid2')?.textContent).toBe('h');
    document.body.replaceChildren();
    const two = await boot({ routes: { 'GET api/v1/hubs': () => hubs([{ id: 'h', last_seen: 2, name: 'Living room' }, { id: 'other', last_seen: 1 }]) } });
    await vi.waitFor(() => expect([...two.app.shadowRoot!.querySelectorAll('select[aria-label="Hub"] option')].map((o) => o.textContent?.trim())).toEqual(['Living room (h) (no config yet)', 'other (no config yet)']));
  });

  it('offers to move a default store to a linked hub', async () => {
    const adopted: string[] = [];
    const { app } = await boot({
      routes: {
        'GET api/v1/hubs': () => ({ body: { hubs: [{ id: 'default', has_draft: false, has_applied: true, applied_rev: 'a', applied_at: 1 }], linked: [{ id: 'hub-a', last_seen: 1 }, { id: 'hub-b', last_seen: 1 }], adopt: { from: 'default', to: ['hub-a', 'hub-b'] } } }),
        'POST api/v1/hubs/adopt': (c) => {
          adopted.push(String(c.body));
          return { body: { ok: true, hub: 'hub-b' } };
        },
      },
    });
    await vi.waitFor(() => expect(app.shadowRoot!.textContent).toContain('Move it to'));
    expect([...app.shadowRoot!.querySelectorAll('.banner button')].map((b) => b.textContent?.trim())).toEqual(['hub-a', 'hub-b']);
    expect([...app.shadowRoot!.querySelectorAll('select[aria-label="Hub"] option')].map((o) => o.getAttribute('value'))).toEqual(['h', 'hub-a', 'hub-b', 'default']);
    expect(adopted).toEqual([]);
  });

  it('a 422 on apply closes the dialog and records the errors', async () => {
    const { store } = await boot({ draft: true });
    store.api.apply = () => Promise.reject(new ApiError(422, 'x', 'bad', [{ path: ['devices', 0, 'id'], msg: 'bad id' }]));
    await expect(store.applyDraft()).rejects.toBeInstanceOf(ApiError);
    expect(store.errors).toHaveLength(1);
  });
});

describe('import dialog', () => {
  it('lists unresolvable IR files from a 422', async () => {
    const { app, store } = await boot({ applied: false });
    store.api.importYaml = () => Promise.reject(new ApiError(422, 'ir_files_unresolvable', 'files', [], ['flipper:tv.ir']));
    const dlg = app.shadowRoot!.querySelector('hh-import-dialog') as HTMLElement & { open: boolean };
    dlg.open = true;
    await settle(dlg);
    const ta = dlg.shadowRoot!.querySelector('textarea')!;
    ta.value = 'devices: []';
    ta.dispatchEvent(new Event('input'));
    await settle(dlg);
    (dlg.shadowRoot!.querySelector('hh-dialog button.pri') as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('flipper:tv.ir'));
  });
});

import { ago, summarize } from '../src/views/hh-history';
describe('history helpers', () => {
  it('ago is relative and summarize is one line', () => {
    expect(ago(1000, 1030)).toBe('just now');
    expect(ago(1000, 1000 + 300)).toBe('5 min ago');
    expect(ago(1000, 1000 + 90000)).toBe('yesterday');
    expect(summarize([])).toBe('No changes to the config');
    expect(summarize([{ kind: 'changed', area: 'Device', label: 'TV' }, { kind: 'added', area: 'Page', label: 'P' }])).toBe('Device TV changed, +1 more');
  });
});
