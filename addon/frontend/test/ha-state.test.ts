import { describe, expect, it } from 'vitest';
import { Store } from '../src/lib/store';
import { StudioState } from '../src/lib/studio-state';
import { fakeApi } from './helpers';

const cat = (n: number) => ({ entities: [{ entity_id: `light.l${n}`, name: `L${n}`, domain: 'light' }], areas: [], registries: true });
const settle = () => new Promise((r) => setTimeout(r, 30));

describe('loading the Home Assistant list', () => {
  it('a diagnostic toggle or Retry that arrives mid-load runs again afterwards, with the latest flag', async () => {
    let release: () => void = () => undefined;
    const gate = new Promise<void>((r) => (release = r));
    let n = 0;
    const f = fakeApi({
      'GET api/v1/ha/entities': async () => {
        const mine = ++n;
        if (mine === 1) await gate;
        return { body: cat(mine) };
      },
      'GET api/v1/ha/services': () => ({ body: { services: [] } }),
    });
    const st = new StudioState(new Store(f.api, 'h', 'c', 600));
    const first = st.loadHa();
    st.setHaDiag(true); // while the first request is still in flight
    release();
    await first;
    await settle();
    const urls = f.calls.filter((c) => c.url.includes('/ha/entities')).map((c) => c.url);
    expect(urls.length).toBe(2);
    expect(urls[1]).toContain('diagnostic=1');
    expect(st.entities?.[0]?.entity_id).toBe('light.l2');
    expect(st.haLoading).toBe(false);
  });
  it('refreshHa reloads only when the list is older than the limit; the state flag is off unless asked', async () => {
    const f = fakeApi({ 'GET api/v1/ha/entities': () => ({ body: cat(1) }), 'GET api/v1/ha/services': () => ({ body: { services: [] } }) });
    const st = new StudioState(new Store(f.api, 'h', 'c', 600));
    await st.loadHa();
    st.refreshHa(60000);
    await settle();
    expect(f.calls.filter((c) => c.url.includes('/ha/entities')).length).toBe(1);
    expect(f.calls[0]?.url).not.toContain('state=1');
    st.haAt -= 61000;
    st.refreshHa(60000);
    await settle();
    expect(f.calls.filter((c) => c.url.includes('/ha/entities')).length).toBe(2);
  });
});
