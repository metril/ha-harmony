import { beforeEach, describe, expect, it, vi } from 'vitest';
import '../src/components/hh-app';
import '../src/components/hh-apply-dialog';
import '../src/components/hh-import-dialog';
import '../src/components/hh-draft-bar';
import { Store, setStore } from '../src/lib/store';
import { ApiError } from '../src/lib/api';
import { fakeLive } from './fake-ws';
import { REVS, fakeApi, libRoutes, previewBad, previewOk, status, tvConfig, type Handler } from './helpers';

async function boot(opts: { draft?: boolean; applied?: boolean; errors?: boolean; hash?: string; statuses?: object[]; hubs?: { id: string }[]; linked?: { id: string; last_seen: number; name?: string }[]; routes?: Record<string, Handler>; config?: ReturnType<typeof tvConfig> } = {}) {
  const statuses = [...(opts.statuses ?? [status()])];
  const cfg = opts.config ?? tvConfig();
  const f = fakeApi({
    ...libRoutes(opts.draft ? { config: cfg } : opts.applied !== false ? { config: tvConfig() } : null, { revs: opts.applied !== false, applied: opts.applied !== false }),
    'POST api/v1/config/preview': () =>
      opts.errors
        ? { body: previewBad([{ path: ['devices', 0, 'commands', 'cmd_3'], msg: 'nec cmd 999 outside 0..255', line: null }]) }
        : { body: previewOk() },
    'GET api/v1/hubs': () => ({ body: { hubs: (opts.hubs ?? [{ id: 'h' }]).map((h) => ({ has_draft: false, has_applied: true, applied_rev: 'aaaa1111', applied_at: 1, ...h })) } }),
    'POST api/v1/hubs/h/apply': () => ({ body: { ok: true, hub: 'h', applied: { config_id: 'c', rev: 4, name: 'Main', at: 1_700_000_200, hash: 'cafebabe' }, history: [{ config_id: 'c', rev: 3, name: 'Main', at: 1_700_000_000, hash: 'abcdef0123456789' }], history_max: 5 } }),
    'GET api/v1/configs/c/revs/2': () => ({ body: { ...REVS[1], id: 'c', config: { ...tvConfig(), activities: [] } } }),
    'POST api/v1/ir/test': () => ({ body: { ok: true, action: {} } }),
    ...(opts.routes ?? {}),
  });
  // the live topics the add-on would push: `hubs` is what `GET hubs` answers (plus who is linked), `hub:h` the config status
  const hubsBody = ((await f.api.hubs()) as unknown) as object;
  fakeLive.topics['hubs'] = { adopt: null, now: 1000, ...hubsBody, linked: opts.linked ?? ((hubsBody as { linked: unknown[] }).linked.length ? (hubsBody as { linked: unknown[] }).linked : [{ id: 'h', last_seen: 1 }]) };
  fakeLive.topics['hub:h'] = statuses[0];
  fakeLive.topics['configs'] = { configs: [{ id: 'c', name: 'Main', created: 1, updated: 2, latest_rev: 3, rev_count: 3, draft_rev: 7, has_draft: true, draft_updated: 1, base_rev: 3, dirty: false, hubs: [{ hub: 'h', rev: 3 }] }] };
  const pushStatus = (st: object) => fakeLive.push({ t: 'hub', ...st });
  const store = new Store(f.api, 'h', 'c', 600);
  setStore(store);
  await store.load();
  location.hash = opts.hash ?? '#/overview';
  const app = document.createElement('hh-app') as HTMLElement & { updateComplete: Promise<unknown> };
  document.body.append(app);
  await app.updateComplete;
  return { app, store, pushStatus, ...f };
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
    expect(text(bar)).toContain('Rev 3 · No Changes');
    expect(text(bar)).toContain('Valid');
  });

  it('first-run shows the empty state with Import and Add Your First Device', async () => {
    const { app } = await boot({ applied: false });
    const ov = await lazyEl(app, 'hh-overview');
    await settle(ov as HTMLElement);
    expect(text(ov)).toContain('Import YAML File');
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
    ([...dv.shadowRoot!.querySelectorAll('.seg button')].find((b) => b.textContent === 'List View') as HTMLButtonElement).click();
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('tbody tr').length).toBe(61);
  });

  describe('devices: off_ends_activity toggle', () => {
    const haCfg = (extra: object = {}) => {
      const c = tvConfig();
      c.devices!.push({ id: 'shield', name: 'Shield', kind: 'ha', entity: 'media_player.shield', ...extra } as never);
      return c;
    };
    const open = async (hash: string, config: ReturnType<typeof tvConfig>) => {
      const { app, store } = await boot({ hash, config, draft: true, routes: { 'GET api/v1/health': () => ({ body: { ok: true, addon_version: '1', capabilities: { ha_actions: true } } }) } });
      const dv = await lazyEl(app, 'hh-devices');
      await settle(dv as HTMLElement);
      await vi.waitFor(() => expect((dv as unknown as { ha: { haWritable: boolean } }).ha.haWritable).toBe(true));
      await settle(dv as HTMLElement);
      return { dv, store };
    };
    const box = (dv: HTMLElement) => dv.shadowRoot!.querySelector('input[role="switch"][type="checkbox"]:not([aria-label])') as HTMLInputElement | null;
    const dev = (store: { config: { devices?: { id: string }[] } }) => store.config.devices!.find((d) => d.id === 'shield') as Record<string, unknown>;

    it('renders for HA devices only', async () => {
      expect(box((await open('#/devices/shield', haCfg())).dv)).not.toBeNull();
      document.body.replaceChildren();
      expect(box((await open('#/devices/tv', haCfg())).dv)).toBeNull();
    });
    it('sets and removes the key', async () => {
      const { dv, store } = await open('#/devices/shield', haCfg());
      const b = box(dv)!;
      expect(b.disabled).toBe(false);
      b.checked = true;
      b.dispatchEvent(new Event('change'));
      await vi.waitFor(() => expect(dev(store as never).off_ends_activity).toBe(true));
      await settle(dv);
      const b2 = box(dv)!;
      b2.checked = false;
      b2.dispatchEvent(new Event('change'));
      await vi.waitFor(() => expect('off_ends_activity' in dev(store as never)).toBe(false));
    });
    it('is disabled without an entity; clearing the entity drops the key', async () => {
      const { dv, store } = await open('#/devices/shield', haCfg({ off_ends_activity: true }));
      expect(box(dv)!.checked).toBe(true);
      const ent = dv.shadowRoot!.querySelector('input[aria-label="Entity"]') as HTMLInputElement;
      ent.value = '';
      ent.dispatchEvent(new Event('change'));
      await vi.waitFor(() => expect('entity' in dev(store as never) || 'off_ends_activity' in dev(store as never)).toBe(false));
      await settle(dv);
      expect(box(dv)!.disabled).toBe(true);
      expect(box(dv)!.checked).toBe(false);
    });
  });

  it('devices gallery: a card per device with power status, counts and source, plus Add Device', async () => {
    const { app } = await boot({ hash: '#/devices' });
    const dv = await lazyEl(app, 'hh-devices');
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('.dcard').length).toBe(2);
    const t = text(dv);
    expect(t).toContain('Power On + Off');
    expect(t).toContain('Power Toggle Only');
    expect(t).toContain('61 commands');
    expect(t).toContain('IR archive · Samsung UE55');
    expect(t).toContain('Your Harmony backup');
    expect(dv.shadowRoot!.querySelector('a.add')!.getAttribute('href')).toBe('#/devices/new');
  });

  it('revisions: the hub status sits above the list, the applied rev has an Applied On badge, every rev has its actions', async () => {
    const { app } = await boot({ hash: '#/history', statuses: [status({ applied_rev: 'a', acked_rev: 'a', result: 'ok' })] });
    const h = await lazyEl(app, 'hh-history');
    await settle(h as HTMLElement);
    expect(text(h)).toContain('Rev 3');
    await vi.waitFor(() => expect(text(h)).toContain('On the Remote'));
    for (const old of ['Acknowledged', 'Current']) expect(text(h)).not.toContain(old);
    expect(h.shadowRoot!.querySelectorAll('#hub-runs .chip').length).toBe(1);
    const rows = [...h.shadowRoot!.querySelectorAll('li')];
    expect(rows).toHaveLength(3);
    expect(rows[0]!.textContent).toContain('Rev 3'); // newest first
    expect(rows[0]!.textContent).toContain('Applied On h');
    expect(rows[0]!.textContent).toContain('Evening scene');
    expect(rows[1]!.textContent).not.toContain('Applied On');
    expect([...rows[1]!.querySelectorAll('.acts > *')].map((b) => b.textContent!.trim())).toEqual(['Compare', 'Restore To Draft', 'Duplicate As New Config', 'Apply To Hub', 'Export YAML']);
    expect(rows[1]!.querySelector('time')!.getAttribute('title')).toBeTruthy();
    expect(rows[1]!.querySelector('a')!.getAttribute('href')).toContain('/configs/c/export_yaml?source=rev%3A2');
  });

  it('history: sending, not sent, and rejected with a reason behind See Why', async () => {
    const live = { applied_rev: 'aaaa1111' };
    const a = await boot({ hash: '#/history', statuses: [status(live)] });
    const h1 = await lazyEl(a.app, 'hh-history');
    await vi.waitFor(() => expect(text(h1)).toContain('Sending…'));
    document.body.replaceChildren();
    const b = await boot({ hash: '#/history', statuses: [status({ connected: false })], linked: [] });
    const h2 = await lazyEl(b.app, 'hh-history');
    await vi.waitFor(() => expect(text(h2)).toContain('Not Sent'));
    document.body.replaceChildren();
    const c = await boot({ hash: '#/history', statuses: [status({ ...live, acked_rev: 'aaaa1111', result: 'error', detail: 'bad key map' })] });
    const h3 = await lazyEl(c.app, 'hh-history');
    await vi.waitFor(() => expect(text(h3)).toContain('Rejected By Hub'));
    expect(text(h3)).not.toContain('bad key map');
    const why = [...h3.shadowRoot!.querySelectorAll('button')].find((x) => x.textContent!.trim() === 'See Why') as HTMLButtonElement;
    why.click();
    await vi.waitFor(() => expect(h3.shadowRoot!.querySelector('#why')?.textContent).toContain('bad key map'));
  });

  it('header shows the hub and whether Home Assistant is connected for it', async () => {
    const { app } = await boot({ statuses: [status()] });
    const hub = app.shadowRoot!.querySelector('header .hub')!;
    expect(hub.textContent).toContain('Hub:');
    expect(hub.textContent).toContain('h');
    await vi.waitFor(() => expect(hub.textContent).toContain('Linked, connected'));
  });

  it('header hub state: not linked at all', async () => {
    const { app } = await boot({ statuses: [status({ connected: false })], linked: [] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('header .hub')!.textContent).toContain('No Home Assistant linked'));
  });

  it('history: Compare loads the old version and lists what differs from the current one', async () => {
    const { app, calls } = await boot({ hash: '#/history' });
    const h = await lazyEl(app, 'hh-history');
    await settle(h as HTMLElement);
    (h.shadowRoot!.querySelector('button[aria-label="Compare rev 2"]') as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(h)).toContain('Watch TV'));
    expect(calls.some((c) => c.url.endsWith('/configs/c/revs/2'))).toBe(true);
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
    expect(text(av)).toContain('New Activity');
    location.hash = '#/activities/watch_tv';
    await vi.waitFor(() => expect(text(av)).toContain('Signal Chain'));
    await settle(av as HTMLElement);
    const t = text(av);
    expect(t).toContain('Power On Living room TV');
    expect(t).toContain('2 s');
    expect(t).toContain('Drop a Device');
    expect(t).toContain('Who Handles It');
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
    await vi.waitFor(() => expect(text(av)).toContain('Who Handles It'));
    await settle(av as HTMLElement);
    av.shadowRoot!.querySelector<HTMLElement>('button[aria-label^="Who handles volume"]')!.click();
    await settle(av as HTMLElement);
    const items = [...av.shadowRoot!.querySelectorAll<HTMLElement>('.zmenu button')];
    items.find((b) => b.textContent?.includes('Living Room TV'))!.click();
    expect(store.config.activities![0]!.keys).toMatchObject({ volume_up: { device: 'tv', command: 'vol_up' }, volume_down: { device: 'tv', command: 'vol_down' } });
    expect((store.config.editor as { activities: Record<string, { handlers: object }> }).activities['watch_tv']!.handlers).toEqual({ volume: 'tv' });
    await settle(av as HTMLElement);
    expect(text(av)).toContain('No match for Volume Mute');
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
    await click('.tile', 'IR Archive');
    await vi.waitFor(() => expect(w.shadowRoot!.querySelector('.brand')).toBeTruthy());
    await click('.brand', 'Sony');
    await click('.tile', 'TV');
    await vi.waitFor(() => expect(text(w)).toContain('Code 1 of 2'));
    await click('button', 'No, Next Code');
    await vi.waitFor(() => expect(text(w)).toContain('Code 2 of 2'));
    await click('button', 'Yes');
    await vi.waitFor(() => expect(text(w)).toContain('Likely Matches'));
    await click('.likely .btn', 'KDL-1');
    await vi.waitFor(() => expect(text(w)).toContain('12 gaps shortened'));
    expect(text(w)).toContain('weird: unsupported');
    expect(calls.some((c) => c.method === 'PUT')).toBe(false);
    await click('.side .btn.pri', 'Add Device');
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
    ([...w.shadowRoot!.querySelectorAll<HTMLElement>('.tile')].find((e) => e.textContent?.includes('IR Archive')))!.click();
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
    expect(text(bar)).toContain('1 error');
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
    expect(dlg.shadowRoot!.querySelector('hh-dialog')?.getAttribute('heading')).toBe('Apply Draft to h?');
    expect(text(dlg)).toContain('Renamed TV');
    const apply = [...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply To Hub')) as HTMLButtonElement;
    apply.click();
    await vi.waitFor(() => expect(dlg.shadowRoot!.querySelector('hh-dialog')?.getAttribute('heading')).toBe('Rev 4 applied to h'));
    expect(text(dlg)).toContain('Saved as rev 4');
    expect(store.api).toBeDefined();
    expect(text(dlg)).toContain('Sending to the Remote (pending)');
    expect(store.errors).toEqual([]);
  });

  it('forgets the "apply anyway" confirmation when the dialog opens again', async () => {
    const { app } = await boot({ draft: true });
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; cutOk: boolean };
    dlg.cutOk = true;
    dlg.open = true;
    await settle(dlg);
    expect(dlg.cutOk).toBe(false);
  });

  it('moves from waiting to done when a status push carries the ack of the applied bundle rev', async () => {
    const { app, store, pushStatus } = await boot({ draft: true, statuses: [status()] });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean };
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply To Hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('Sending to the Remote (pending)'));
    pushStatus(status({ acked_rev: 'old00000', result: 'ok' })); // an ack of another rev does not count
    await settle(dlg);
    expect(text(dlg)).toContain('(pending)');
    pushStatus(status({ acked_rev: 'aaaa1111', result: 'ok' }));
    await vi.waitFor(() => expect(text(dlg)).not.toContain('(pending)'));
    expect(text(dlg)).toContain('On the Remote');
    expect(text(dlg)).toContain('Done');
  });

  it('a rejecting ack shows the hub detail', async () => {
    const { app, store, pushStatus } = await boot({ draft: true });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean };
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply To Hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('(pending)'));
    pushStatus(status({ acked_rev: 'aaaa1111', result: 'error', detail: 'bad key map' }));
    await vi.waitFor(() => expect(text(dlg)).toContain('bad key map'));
  });

  it('after the UI deadline it says Home Assistant has not picked it up, and still completes on a late ack', async () => {
    const { app, store, pushStatus } = await boot({ draft: true, statuses: [status({ connected: false })], linked: [] });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; waitMs: number };
    dlg.waitMs = 30;
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply To Hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('No Home Assistant integration is linked'));
    pushStatus(status({ acked_rev: 'aaaa1111', result: 'ok' }));
    await vi.waitFor(() => expect(text(dlg)).toContain('On the Remote'));
  });

  it('names the hub id when Home Assistant is linked for another one, instead of waiting forever', async () => {
    const { app, store } = await boot({ draft: true, statuses: [status({ connected: false })], linked: [{ id: 'living-room-hub', last_seen: 995 }] });
    store.edit((c) => (c.devices![0]!.name = 'Renamed TV'));
    await store.flush();
    const dlg = app.shadowRoot!.querySelector('hh-apply-dialog') as HTMLElement & { open: boolean; waitMs: number };
    dlg.waitMs = 30;
    dlg.open = true;
    await settle(dlg);
    ([...dlg.shadowRoot!.querySelectorAll('hh-dialog button')].find((b) => b.textContent?.includes('Apply To Hub')) as HTMLButtonElement).click();
    await vi.waitFor(() => expect(text(dlg)).toContain('not connected for this hub (h)'));
    expect(text(dlg)).toContain('living-room-hub');
  });

  it('the app shows applied vs acked after navigation, and the link problem', async () => {
    const { app } = await boot({ statuses: [status()] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('#link-pending')?.textContent).toContain('Rev 3 is being sent to the remote'));
    expect(app.shadowRoot!.querySelector('#link-problem')).toBeNull();
    document.body.replaceChildren();
    const stale = await boot({ statuses: [status({ connected: false })] });
    await vi.waitFor(() => expect(stale.app.shadowRoot!.querySelector('#link-problem')?.textContent).toContain('not connected for this hub (h)'));
    document.body.replaceChildren();
    const none = await boot({ statuses: [status({ connected: false })], linked: [] });
    await vi.waitFor(() => expect(none.app.shadowRoot!.querySelector('#link-problem')?.textContent).toContain('No Home Assistant integration is linked'));
  });

  it('a hub rejection stays on screen after the dialog is gone, with its detail', async () => {
    const { app } = await boot({ statuses: [status({ acked_rev: 'aaaa1111', result: 'error', detail: 'pages[1].widgets[3].icon: unknown icon' })] });
    await vi.waitFor(() => expect(app.shadowRoot!.querySelector('#link-rejected')?.textContent).toContain('unknown icon'));
    expect(app.shadowRoot!.querySelector('#link-pending')).toBeNull();
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
    store.api.applyToHub = () => Promise.reject(new ApiError(422, 'x', 'bad', [{ path: ['devices', 0, 'id'], msg: 'bad id' }]));
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

describe('no polling', () => {
  it('an idle editor makes no fetch after boot, and no XHR', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'setInterval'] });
    try {
      const xhr = vi.spyOn(XMLHttpRequest.prototype, 'send');
      const { calls } = await boot({ draft: true });
      await vi.advanceTimersByTimeAsync(1000);
      const n = calls.length;
      await vi.advanceTimersByTimeAsync(10 * 60 * 1000);
      expect(calls.length).toBe(n);
      expect(xhr).not.toHaveBeenCalled();
    } finally {
      vi.useRealTimers();
    }
  });
});

describe('draft bar issues chip', () => {
  const chk = (msg: string, page?: number, index?: number) => ({ kind: 'action' as const, msg, ...(page !== undefined ? { page } : {}), ...(index !== undefined ? { index } : {}) });
  const bar = async (app: HTMLElement) => {
    const b = app.shadowRoot!.querySelector('hh-draft-bar') as HTMLElement;
    await settle(b);
    return b;
  };
  it('reads Valid with no issues', async () => {
    const { app } = await boot();
    const b = await bar(app);
    expect(b.shadowRoot!.querySelector('.chip.ok')?.textContent?.trim()).toBe('Valid');
    expect(b.shadowRoot!.querySelector('#issues-pop')).toBeNull();
  });
  it('one chip for 1 error and 2 checks, opening a list with Show only where there is a target', async () => {
    const { app, store } = await boot({ draft: true, errors: true });
    await settle(app);
    const long = 'x'.repeat(200);
    store.setChecks([chk(long, 0, 1), chk('device glyph')]);
    const b = await bar(app);
    const chip = b.shadowRoot!.querySelector('#issues-chip') as HTMLButtonElement;
    expect(chip.textContent?.replace(/\s+/g, ' ').trim()).toBe('✗ 1 error · 2 warnings');
    expect(chip.classList.contains('bad')).toBe(true);
    chip.click();
    await settle(b);
    const pop = b.shadowRoot!.querySelector('[role=dialog]#issues-pop')!;
    const rows = [...pop.querySelectorAll('.row')];
    expect(rows).toHaveLength(3);
    expect(rows[1]!.textContent).toContain(long);
    expect(rows[0]!.querySelector('button')).toBeTruthy();
    expect(rows[1]!.querySelector('button')).toBeTruthy();
    expect(rows[2]!.querySelector('button')).toBeNull();
    const cs = getComputedStyle(rows[1]!.querySelector('.msg')!);
    expect(cs.whiteSpace).not.toBe('nowrap');
    expect(cs.textOverflow).not.toBe('ellipsis');
  });
  it('warnings only is a warn chip; Escape and an outside click close and refocus', async () => {
    const { app, store } = await boot();
    store.setChecks([chk('a', 0, 0), chk('b', 0, 1)]);
    const b = await bar(app);
    const chip = b.shadowRoot!.querySelector('#issues-chip') as HTMLButtonElement;
    expect(chip.textContent?.replace(/\s+/g, ' ').trim()).toBe('⚠ 2 warnings');
    expect(chip.classList.contains('warn')).toBe(true);
    chip.click();
    await settle(b);
    chip.focus();
    chip.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, composed: true }));
    await settle(b);
    expect(b.shadowRoot!.querySelector('#issues-pop')).toBeNull();
    expect(b.shadowRoot!.activeElement).toBe(chip);
    chip.click();
    await settle(b);
    const other = document.createElement('button');
    document.body.append(other);
    other.focus();
    other.click();
    await settle(b);
    expect(b.shadowRoot!.querySelector('#issues-pop')).toBeNull();
    expect(document.activeElement).toBe(other);
    other.remove();
  });
  it('chip glyph is aria-hidden, Show buttons are labelled', async () => {
    const { app, store } = await boot();
    store.setChecks([chk('device glyph', 0, 1)]);
    const b = await bar(app);
    const chip = b.shadowRoot!.querySelector('#issues-chip') as HTMLButtonElement;
    expect(chip.querySelector('[aria-hidden="true"]')?.textContent).toBe('⚠');
    expect(chip.textContent?.replace(/\s+/g, ' ').trim()).toContain('1 warning');
    chip.click();
    await settle(b);
    expect(b.shadowRoot!.querySelector('#issues-pop .row button')?.getAttribute('aria-label')).toMatch(/^Show: .*device glyph/);
  });
  it('popover stays closed when issues clear then reappear', async () => {
    const { app, store } = await boot();
    store.setChecks([chk('a')]);
    const b = await bar(app);
    (b.shadowRoot!.querySelector('#issues-chip') as HTMLElement).click();
    await settle(b);
    expect(b.shadowRoot!.querySelector('#issues-pop')).not.toBeNull();
    store.setChecks([]);
    await settle(b);
    store.setChecks([chk('b')]);
    await settle(b);
    expect(b.shadowRoot!.querySelector('#issues-pop')).toBeNull();
  });
  it('Show on a check for a deleted page does not throw', async () => {
    const { app, store } = await boot();
    store.setChecks([chk('gone', 99, 0)]);
    const b = await bar(app);
    (b.shadowRoot!.querySelector('#issues-chip') as HTMLElement).click();
    await settle(b);
    const show = b.shadowRoot!.querySelector('#issues-pop .row button') as HTMLButtonElement;
    expect(() => show.click()).not.toThrow();
  });
});

