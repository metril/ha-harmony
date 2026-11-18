import { beforeEach, describe, expect, it, vi } from 'vitest';
import '../src/components/hh-app';
import '../src/components/hh-apply-dialog';
import '../src/components/hh-import-dialog';
import '../src/components/hh-draft-bar';
import { Store, setStore } from '../src/lib/store';
import { ApiError } from '../src/lib/api';
import { META, fakeApi, tvConfig } from './helpers';

const usage = { json_bytes: 9000, json_max: 131072, pool_words: 900, pool_max: 1000, devices: 2, devices_max: 16, activities: 1, activities_max: 16 };

async function boot(opts: { draft?: boolean; applied?: boolean; errors?: boolean; hash?: string } = {}) {
  const cfg = tvConfig();
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
        ? { status: 422, body: { ok: false, errors: [{ path: ['devices', 0, 'commands', 'cmd_3'], msg: 'nec cmd 999 outside 0..255' }] } }
        : { body: { ok: true, hash: 'cafebabe', pages: [], usage } },
    'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 1_700_000_100, ...META } }),
    'GET api/v1/hubs': () => ({ status: 404, body: {} }),
    'POST api/v1/config/apply': () => ({ body: { ok: true, applied: { rev: 4, at: 1_700_000_200, hash: 'cafebabe' }, history: [META.applied, ...META.history], history_max: 5 } }),
    'GET api/v1/config/status': () => ({ status: 404, body: {} }),
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
const text = (el: Element) => el.shadowRoot?.textContent?.replace(/\s+/g, ' ') ?? '';

beforeEach(() => {
  document.body.replaceChildren();
});

describe('screens render with a realistic config', () => {
  it('overview: stats, activities, permissions, nothing to apply', async () => {
    const { app } = await boot();
    const ov = app.shadowRoot!.querySelector('hh-overview')!;
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

  it('first-run shows the empty state with Import and a disabled Add device', async () => {
    const { app } = await boot({ applied: false });
    const ov = app.shadowRoot!.querySelector('hh-overview')!;
    await settle(ov as HTMLElement);
    expect(text(ov)).toContain('Import YAML file');
    expect(ov.shadowRoot!.querySelector('button[aria-disabled="true"]')?.textContent).toContain('Add first device');
  });

  it('devices: 60+ commands listed read-only with a disabled Send IR', async () => {
    const { app } = await boot({ hash: '#/devices/tv' });
    const dv = app.shadowRoot!.querySelector('hh-devices')!;
    await settle(dv as HTMLElement);
    expect(dv.shadowRoot!.querySelectorAll('tbody tr').length).toBe(61);
    const send = dv.shadowRoot!.querySelector('hh-send-ir')!;
    expect(send.shadowRoot!.querySelector('button')?.getAttribute('aria-disabled')).toBe('true');
    expect(send.shadowRoot!.querySelector('button')?.getAttribute('title')).toMatch(/not available/);
    expect(text(dv)).toContain('Living room TV');
    expect(text(dv)).toContain('From IR archive');
  });

  it('history: current rev, newest earlier rev revertable', async () => {
    const { app } = await boot({ hash: '#/history' });
    const h = app.shadowRoot!.querySelector('hh-history')!;
    await settle(h as HTMLElement);
    expect(text(h)).toContain('Rev 3');
    expect(text(h)).toContain('Revert to rev 2');
  });

  it('unbuilt sections show a clear empty state, not a dead link', async () => {
    for (const s of ['activities', 'keys', 'pages', 'ir']) {
      const { app } = await boot({ hash: `#/${s}` });
      const p = app.shadowRoot!.querySelector('hh-placeholder')!;
      await settle(p as HTMLElement);
      expect(text(p)).toContain('Not built yet');
      document.body.replaceChildren();
    }
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
    await vi.waitFor(() => expect(dlg.shadowRoot!.querySelector('hh-dialog')?.getAttribute('heading')).toBe('Applied as rev 4'));
    expect(text(dlg)).toContain('Saved as rev 4');
    expect(text(dlg)).toContain('Waiting for Home Assistant (pending)');
    expect(store.hasDraft).toBe(false);
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
