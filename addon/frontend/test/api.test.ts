import { describe, expect, it } from 'vitest';
import { Api, ApiError, DraftQueue, haEntitiesOrNull } from '../src/lib/api';
import { META, fakeApi, previewBad } from './helpers';
import { fakeLive } from './fake-ws';

describe('api client', () => {
  it('builds relative URLs under the ingress base path', async () => {
    const { api, calls } = fakeApi({ 'GET api/v1/config': () => ({ body: { hub: 'h', draft: null, ...META } }) });
    await api.getConfig('sim_hub', true);
    expect(calls[0]?.url).toBe('https://ha.example/api/hassio_ingress/tok/api/v1/config?hub=sim_hub&full=1');
    expect(new Api('https://x.test/a/b/').href('/api/v1/health')).toBe('https://x.test/a/b/api/v1/health');
  });

  it('sends the CSRF header and JSON type on mutating requests only', async () => {
    const { api, calls } = fakeApi({
      'GET api/v1/health': () => ({ body: { ok: true } }),
      'PUT api/v1/config/draft': () => ({ body: { hub: 'h', draft_updated: 1, ...META } }),
      'POST api/v1/config/apply': () => ({ body: { ok: true, ...META } }),
    });
    await api.health();
    await api.putDraft('h', {});
    await api.apply('h');
    expect(calls[0]?.headers['X-HH-CSRF']).toBeUndefined();
    for (const c of calls.slice(1)) {
      expect(c.headers['X-HH-CSRF']).toBe('1');
      expect(c.headers['Content-Type']).toBe('application/json');
    }
    expect(calls[2]?.body).toBe('{}'); // an empty JSON body, which the server requires on POST
  });

  it('imports with the YAML content type and returns typed errors', async () => {
    const { api, calls } = fakeApi({
      'POST api/v1/config/import_yaml': () => ({
        status: 422,
        body: { error: { code: 'ir_files_unresolvable', msg: 'files', files: ['flipper:a.ir'] } },
      }),
    });
    const err = await api.importYaml('h', 'devices: []').catch((e) => e);
    expect(calls[0]?.headers['Content-Type']).toBe('text/yaml');
    expect(calls[0]?.headers['X-HH-CSRF']).toBe('1');
    expect(err).toBeInstanceOf(ApiError);
    expect(err.code).toBe('ir_files_unresolvable');
    expect(err.files).toEqual(['flipper:a.ir']);
  });

  it('preview is a 200 carrying errors, never an exception', async () => {
    const errors = [{ path: ['devices', 0, 'id'], msg: 'bad id', line: null }];
    const { api } = fakeApi({ 'POST api/v1/config/preview': () => ({ body: previewBad(errors) }) });
    const p = await api.preview('h');
    expect(p.ok).toBe(false);
    expect(p.errors).toEqual(errors);
    expect(p.usage).toBeNull();
  });

  it("apply's flat 422 {ok:false, errors} becomes ApiError.errors", async () => {
    const errors = [{ path: ['keys', 'select'], msg: 'unknown', line: null }];
    const { api } = fakeApi({ 'POST api/v1/config/apply': () => ({ status: 422, body: { ok: false, errors } }) });
    const err = await api.apply('h').catch((e) => e);
    expect(err.status).toBe(422);
    expect(err.errors).toEqual(errors);
  });

  it('keeps conflict rev and the 429 cooldown from the nested error', async () => {
    const { api } = fakeApi({
      'PUT api/v1/config/draft': () => ({ status: 409, body: { error: { code: 'conflict', msg: 'stale', rev: 12 } } }),
      'POST api/v1/ir/test': () => ({ status: 429, body: { error: { code: 'rate_limited', msg: 'slow', retry_after_ms: 1500 } } }),
    });
    const c = await api.putDraft('h', {}, 3).catch((e) => e);
    expect(c.isConflict).toBe(true);
    expect(c.extra.rev).toBe(12);
    const r = await api.irTest('h', { proto: 'nec', addr: 1, cmd: 2 }, 3).catch((e) => e);
    expect(r.retryAfterMs).toBe(1500);
  });

  it('ir/test sends {hub, ir, port_mask}; ha lists use the real wrappers', async () => {
    const { api, calls } = fakeApi({ 'POST api/v1/ir/test': () => ({ body: { ok: true, action: {} } }) });
    await api.irTest('h', { proto: 'nec', addr: 1, cmd: 2 }, 3);
    expect(JSON.parse(calls[0]?.body ?? '')).toEqual({ hub: 'h', ir: { proto: 'nec', addr: 1, cmd: 2 }, port_mask: 3 });
  });

  it('turns a network failure into a status 0 error', async () => {
    const api = new Api('https://x.test/', (async () => {
      throw new TypeError('offline');
    }) as typeof fetch);
    const err = await api.health().catch((e) => e);
    expect(err.isNetwork).toBe(true);
  });

  it('Home Assistant lookups fall back to null on 503', async () => {
    const { api } = fakeApi({ 'GET api/v1/ha/entities': () => ({ status: 503, body: { error: { code: 'ha_unavailable', msg: 'x' } } }) });
    expect(await haEntitiesOrNull(api, 'light')).toBeNull();
  });
});

describe('draft queue', () => {
  it('never has two PUTs in flight and collapses queued edits to the newest', async () => {
    let inflight = 0;
    let maxInflight = 0;
    const bodies: unknown[] = [];
    const { api } = fakeApi({
      'PUT api/v1/config/draft': async (c) => {
        inflight++;
        maxInflight = Math.max(maxInflight, inflight);
        bodies.push(JSON.parse(c.body ?? '{}').config);
        await new Promise((r) => setTimeout(r, 5));
        inflight--;
        return { body: { hub: 'h', draft_updated: 1, ...META, rev: bodies.length } };
      },
    });
    const q = new DraftQueue(api, 'h');
    const all = [q.enqueue({ a: 1 }), q.enqueue({ a: 2 }), q.enqueue({ a: 3 })];
    await Promise.all(all);
    expect(maxInflight).toBe(1);
    expect(bodies).toEqual([{ a: 1 }, { a: 3 }]); // 2 was superseded
    expect(q.rev).toBe(2);
  });

  it('sends the last seen rev as If-Match, and none before one is known', async () => {
    const seen: (string | undefined)[] = [];
    const { api } = fakeApi({
      'PUT api/v1/config/draft': (c) => {
        seen.push(c.headers['If-Match']);
        return { body: { hub: 'h', draft_updated: 1, ...META, rev: 7 } };
      },
    });
    const q = new DraftQueue(api, 'h');
    await q.enqueue({ a: 1 });
    await q.enqueue({ a: 2 });
    expect(seen).toEqual([undefined, '7']);
  });

  it('surfaces a 409 conflict', async () => {
    const { api } = fakeApi({
      'PUT api/v1/config/draft': () => ({ status: 409, body: { error: { code: 'conflict', msg: 'stale' } } }),
    });
    const q = new DraftQueue(api, 'h');
    q.rev = 3;
    const err = await q.enqueue({}).catch((e) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect(err.isConflict).toBe(true);
  });
});

describe('no polling endpoints', () => {
  it('the client has no status, events or listen calls, and the live socket opens only on subscribe', () => {
    const api = new Api('https://ha.example/x/', (async () => new Response('{}')) as never) as unknown as Record<string, unknown>;
    for (const name of ['applyStatus', 'btStatus', 'btEvents', 'btListen']) expect(api[name]).toBeUndefined();
    expect(fakeLive.sockets).toHaveLength(0);
  });
});
