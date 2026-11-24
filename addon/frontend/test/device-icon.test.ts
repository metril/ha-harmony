import { describe, expect, it } from 'vitest';
import { render } from 'lit';
import '../src/components/hh-icon-picker';
import { deviceArt } from '../src/lib/device-art';
import { patchDevice } from '../src/lib/device-edit';
import { tvConfig } from './helpers';

describe('device icon', () => {
  it('deviceArt draws the chosen stock icon, else the guessed drawing', () => {
    const host = document.createElement('div');
    render(deviceArt({ id: 'tv', name: 'TV', icon: 'music' }), host);
    expect(host.querySelector('svg')?.getAttribute('viewBox')).toBe('0 0 24 24');
    render(deviceArt({ id: 'tv', name: 'TV' }), host);
    expect(host.querySelector('svg')?.getAttribute('viewBox')).toBe('0 0 64 48');
  });

  it('patchDevice sets and clears the icon', () => {
    const cfg = tvConfig();
    const id = cfg.devices?.[0]?.id as string;
    const set = patchDevice(cfg, id, { icon: 'star' });
    expect(set.devices?.[0]?.icon).toBe('star');
    expect(patchDevice(set, id, { icon: undefined }).devices?.[0]?.icon).toBeUndefined();
  });

  it('the picker searches by keyword and reports the choice', async () => {
    const el = document.createElement('hh-icon-picker') as HTMLElement & { value: string; updateComplete: Promise<boolean> };
    document.body.append(el);
    await el.updateComplete;
    const all = el.shadowRoot!.querySelectorAll('.grid button').length;
    const q = el.shadowRoot!.querySelector('input')!;
    q.value = 'television';
    q.dispatchEvent(new Event('input'));
    await el.updateComplete;
    const hits = el.shadowRoot!.querySelectorAll<HTMLButtonElement>('.grid button');
    expect(hits.length).toBeGreaterThan(0);
    expect(hits.length).toBeLessThan(all);
    let got: unknown = 'unset';
    el.addEventListener('hh-icon', (e) => (got = (e as CustomEvent).detail));
    hits[0]!.click();
    expect(got).toBe('tv');
    el.remove();
  });

  it('Suggested buttons are picker cells (not the shared .ic icon class) and keep the reason as a tooltip', async () => {
    const el = document.createElement('hh-icon-picker') as HTMLElement & { command: string; label: string; updateComplete: Promise<boolean> };
    el.command = 'Volume Up';
    el.label = 'Volume Up';
    document.body.append(el);
    await el.updateComplete;
    const sug = el.shadowRoot!.querySelectorAll<HTMLButtonElement>('.sug button');
    expect(sug.length).toBeGreaterThan(0);
    for (const b of sug) {
      expect(b.classList.contains('ic')).toBe(false);
      expect(b.classList.contains('pick')).toBe(true);
      expect(b.title).toContain(': ');
      expect(b.querySelector('.why')).toBeNull();
    }
    el.remove();
  });
});
