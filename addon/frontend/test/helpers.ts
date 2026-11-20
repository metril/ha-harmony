import { Api } from '../src/lib/api';
import type { Config } from '../src/lib/model';

export interface Call {
  method: string;
  url: string;
  headers: Record<string, string>;
  body: string | undefined;
}
export type Handler = (call: Call) => { status?: number; body?: unknown; text?: string } | Promise<{ status?: number; body?: unknown; text?: string }>;

/** A fake fetch: routes by "METHOD path" (query ignored); records every call. */
export function fakeApi(routes: Record<string, Handler>, base = 'https://ha.example/api/hassio_ingress/tok/') {
  const calls: Call[] = [];
  const f = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    const call: Call = { method: init?.method ?? 'GET', url, headers: (init?.headers ?? {}) as Record<string, string>, body: init?.body as string | undefined };
    calls.push(call);
    const key = `${call.method} ${new URL(url).pathname.replace(/^.*?\/api\/hassio_ingress\/tok\//, '')}`;
    const h = routes[key];
    if (!h) return new Response(JSON.stringify({ error: { code: 'http_error', msg: 'nope' } }), { status: 404 });
    const r = await h(call);
    const text = r.text ?? JSON.stringify(r.body ?? {});
    return new Response(text, { status: r.status ?? 200 });
  }) as typeof fetch;
  return { api: new Api(base, f), calls };
}

export function tvConfig(): Config {
  const commands: Record<string, { proto: string; addr: number; cmd: number }> = {};
  for (let i = 0; i < 60; i++) commands[`cmd_${i}`] = { proto: 'nec', addr: 4, cmd: i };
  commands['Menü ⏎'] = { proto: 'nec', addr: 4, cmd: 99 };
  return {
    devices: [
      {
        id: 'tv',
        name: 'Living room TV',
        settle_ms: 2000,
        aliases: ['51887071'],
        power: { on: { proto: 'nec', addr: 4, cmd: 2 }, off: { proto: 'nec', addr: 4, cmd: 3 } },
        inputs: { hdmi1: { proto: 'nec', addr: 4, cmd: 9 }, hdmi2: { proto: 'nec', addr: 4, cmd: 10 }, tuner: { proto: 'nec', addr: 4, cmd: 11 } },
        commands,
        source: { kind: 'harmony_archive', mfr: 'Samsung', model_file: 'UE55' },
      },
      {
        id: 'hdmi_switch',
        name: 'HDMI switch',
        power: { toggle: { pronto: '0000 006D 0001 0000 0018 0018' } },
        inputs: { port1: { proto: 'rc5', addr: 16, cmd: 1 }, port2: { proto: 'rc5', addr: 16, cmd: 2 } },
        commands: { next: { proto: 'rc5', addr: 16, cmd: 3, port_mask: 3 } },
      },
    ],
    activities: [{ id: 'watch_tv', name: 'Watch TV', devices: [{ device: 'tv', input: 'hdmi1' }, { device: 'hdmi_switch', input: 'port1' }] }],
    keys: { volume_up: { device: 'tv', command: 'cmd_1' } },
    pages: [{ id: 'main', cols: 4, rows: 4, widgets: [{ type: 'tile', id: 't', x: 0, y: 0, w: 2, h: 1, label: 'Temp', entity: 'sensor.living_room_temperature' }] }],
  };
}

export const REVS = [
  { rev: 1, at: 1_699_980_000, note: 'First', hash: 'a1', parent: null },
  { rev: 2, at: 1_699_990_000, note: '', hash: 'a2', parent: 1 },
  { rev: 3, at: 1_700_000_000, note: 'Evening scene', hash: 'a3', parent: 2 },
];
/** the library routes for config `c` (3 revisions, draft based on rev 3) and hub `h` (running Main Rev 3) */
export function libRoutes(draft: { config: Config } | null, o: { revs?: boolean; applied?: boolean; base?: Config } = {}): Record<string, Handler> {
  const revs = o.revs === false ? [] : REVS;
  const base = o.revs === false ? null : 3;
  const sum = { id: 'c', name: 'Main', created: 1, updated: 2, latest_rev: revs.length || null, rev_count: revs.length, draft_rev: 7, has_draft: draft !== null, draft_updated: draft ? 1 : null, base_rev: base, dirty: false };
  const applied = o.applied === false ? null : { config_id: 'c', rev: 3, name: 'Main', at: 1_700_000_000, hash: 'abcdef0123456789' };
  return {
    'GET api/v1/configs/c': () => ({ body: { ...sum, hubs: [], revs, draft: draft ? { ...draft, updated: 1, base_rev: base } : null } }),
    'GET api/v1/configs/c/revs/3': () => ({ body: { ...REVS[2], id: 'c', config: o.base ?? tvConfig() } }),
    'GET api/v1/hubs/h': () => ({ body: { hub: 'h', applied, history: applied ? [{ ...applied, rev: 2, at: 1_699_990_000 }] : [], history_max: 5, config_exists: true, ack: null } }),
    'PUT api/v1/configs/c/draft': () => ({ body: PUTRESP }),
  };
}
export const PUTRESP = { id: 'c', draft_updated: 1_700_000_100, draft_rev: 8, base_rev: 3, dirty: true };
export const DRAFT_REV = 7;

export const USAGE = {
  json_bytes: 9000, pool_words: 900, keys: 1, keys_max: 35,
  devices: 2, devices_max: 16, activities: 1, activities_max: 16, commands: 62,
  inputs: 5, config_bytes: 3000, config_budget: 8 * 1024 * 1024, soft_keys: 0, soft_keys_max: 64, pages: 3, page_limit: 32,
};
export const previewOk = (over: object = {}) => ({ ok: true, hash: 'cafebabe', errors: [], missing_pages: [], pages: [], usage: USAGE, ...over });
export const previewBad = (errors: unknown[]) => ({ ok: false, hash: null, errors, missing_pages: [], pages: [], usage: null });
export const status = (over: object = {}) => ({ hub: 'h', applied_rev: 'aaaa1111', acked_rev: null, result: null, detail: null, acked_at: null, connected: true, connected_at: 1, remotes: [], theme_notices: [], now: 1000, ...over });
