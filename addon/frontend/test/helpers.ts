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

export const META = { applied: { rev: 3, at: 1_700_000_000, hash: 'abcdef0123456789' }, history: [{ rev: 2, at: 1_699_990_000, hash: '1111222233334444' }], history_max: 5, rev: 7 };

export const USAGE = {
  json_bytes: 9000, json_max: 131072, pool_words: 900, pool_max: 1000, keys: 1, keys_max: 35,
  devices: 2, devices_max: 16, activities: 1, activities_max: 16, commands: 62, commands_max: 256,
  inputs: 5, inputs_max: 64, config_bytes: null, config_budget: null, soft_keys: 0, soft_keys_max: 64, pages: 3, page_limit: 32,
};
export const previewOk = (over: object = {}) => ({ ok: true, hash: 'cafebabe', errors: [], missing_pages: [], pages: [], usage: USAGE, ...over });
export const previewBad = (errors: unknown[]) => ({ ok: false, hash: null, errors, missing_pages: [], pages: [], usage: null });
export const status = (over: object = {}) => ({ hub: 'h', applied_rev: 'aaaa1111', acked_rev: null, result: null, detail: null, acked_at: null, linked: true, last_seen: 1, ...over });
