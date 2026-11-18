import { describe, expect, it } from 'vitest';
import { Api, ApiError, DraftQueue, haEntitiesOrNull } from '../src/lib/api';
import { META, fakeApi } from './helpers';

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

  it('reads validation errors from a 422 preview', async () => {
    const errors = [{ path: ['devices', 0, 'id'], msg: 'bad id' }];
    const { api } = fakeApi({ 'POST api/v1/config/preview': () => ({ status: 422, body: { ok: false, errors } }) });
    const err = await api.preview('h').catch((e) => e);
    expect(err.status).toBe(422);
    expect(err.errors).toEqual(errors);
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
        return { body: { hub: 'h', draft_updated: 1, draft_rev: bodies.length, ...META } };
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
        return { body: { hub: 'h', draft_updated: 1, draft_rev: 7, ...META } };
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
