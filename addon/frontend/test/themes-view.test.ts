import { afterEach, describe, expect, it } from 'vitest';
import { Store, setStore } from '../src/lib/store';
import { fakeApi, libRoutes, tvConfig } from './helpers';
import '../src/views/hh-themes';
import { HhColourField } from '../src/components/hh-colour-field';
import { presetTheme } from '../src/lib/theme-model';

const settle = async (el: { updateComplete: Promise<unknown> }) => {
  await el.updateComplete;
  await new Promise((r) => setTimeout(r, 0));
  await el.updateComplete;
};
async function mount(cfg = tvConfig()) {
  const f = fakeApi({
    ...libRoutes({ config: cfg }),
    'POST api/v1/config/preview': () => ({ body: { ok: true, hash: 'h', errors: [], missing_pages: [], pages: [], usage: null } }),
  });
  const store = new Store(f.api, 'h', 'c', 20);
  setStore(store);
  await store.load();
  const el = document.createElement('hh-themes') as HTMLElement & { updateComplete: Promise<unknown>; selected: string; notices: string[] };
  document.body.append(el);
  await settle(el);
  return { el, store };
}
afterEach(() => (document.body.innerHTML = ''));
const btn = (el: HTMLElement, label: string) => el.shadowRoot!.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)!;

describe('themes view', () => {
  it('lists the four presets, marks the active one, and shows the degradation notice only when told', async () => {
    const { el } = await mount();
    const cards = [...el.shadowRoot!.querySelectorAll('article')].map((a) => a.getAttribute('aria-label'));
    expect(cards).toEqual(['Cards', 'Midnight', 'Daylight', 'High Contrast']);
    expect(el.shadowRoot!.querySelector('#theme-notice')).toBeNull();
    el.notices = ['page home: custom colours snapped to the palette'];
    await settle(el);
    expect(el.shadowRoot!.querySelector('#theme-notice')?.textContent).toContain('palette colours only');
  });
  it('Use sets theme:, Duplicate creates a user theme, undo takes both back', async () => {
    const { el, store } = await mount();
    btn(el, 'Use Midnight').click();
    expect(store.config.theme).toBe('midnight');
    btn(el, 'Duplicate Midnight').click();
    expect(store.config.themes).toEqual({ my_midnight: { extends: 'midnight' } });
    store.undo();
    store.undo();
    expect(store.config.theme).toBeUndefined();
    expect(store.config.themes).toBeUndefined();
  });
  it('the editor shows tokens and a problems list with Fix All', async () => {
    const cfg = { ...tvConfig(), theme: 'x', themes: { x: { extends: 'midnight', tokens: { tx: '#404848' } } } };
    const { el, store } = await mount(cfg);
    el.selected = 'x';
    await settle(el);
    const root = el.shadowRoot!;
    expect(root.querySelector('#prob-chip')?.textContent).toContain('contrast problem');
    expect(root.querySelectorAll('.tk .pi').length).toBeGreaterThan(0);
    expect(root.querySelectorAll('.tk').length).toBe(13);
    [...root.querySelectorAll<HTMLButtonElement>('.side button')].find((b) => b.textContent === 'Fix All')!.click();
    await settle(el);
    expect(root.querySelector('#prob-chip')).toBeNull();
    expect(store.config.themes?.['x']?.tokens?.['tx']).toMatch(/^#[0-9A-F]{6}$/);
  });
});

describe('colour field', () => {
  async function field(value?: string) {
    const el = new HhColourField();
    el.theme = presetTheme('default');
    if (value) el.value = value;
    document.body.append(el);
    await el.updateComplete;
    return el;
  }
  it('shows Default + 12 palette swatches + Custom, each with its ink', async () => {
    const el = await field('teal');
    expect(el.shadowRoot!.querySelectorAll('.sw')).toHaveLength(14);
    expect(el.shadowRoot!.querySelector('.sw[aria-pressed="true"]')?.getAttribute('aria-label')).toBe('Teal');
  });
  it('a low-contrast custom colour warns and offers the nearest readable one', async () => {
    const el = await field('#777777');
    const read = el.shadowRoot!.querySelector('[role=status]')!;
    expect(read.textContent).toContain('Low contrast');
    expect(read.textContent).toMatch(/label contrast 4\.\d:1/);
    const got: (string | undefined)[] = [];
    el.addEventListener('colour', (e) => got.push((e as CustomEvent<{ value?: string }>).detail.value));
    [...read.querySelectorAll('button')].find((b) => b.textContent?.includes('nearest readable'))!.click();
    expect(got[0]).toMatch(/^#[0-9A-F]{6}$/);
  });
  it('choosing a swatch and Default emit the palette name / undefined', async () => {
    const el = await field();
    const got: (string | undefined)[] = [];
    el.addEventListener('colour', (e) => got.push((e as CustomEvent<{ value?: string }>).detail.value));
    el.shadowRoot!.querySelector<HTMLButtonElement>('.sw[aria-label="Red"]')!.click();
    el.shadowRoot!.querySelector<HTMLButtonElement>('.sw.def')!.click();
    expect(got).toEqual(['red', undefined]);
  });
});

describe('theme editor preview', () => {
  it('clicking an element lists the tokens behind it; hovering a token outlines its uses', async () => {
    const cfg = { ...tvConfig(), themes: { x: { extends: 'default' } } };
    const { el } = await mount(cfg);
    el.selected = 'x';
    await settle(el);
    const root = el.shadowRoot!;
    const svg = root.querySelector('svg.pv')!;
    expect(svg.getBoundingClientRect).toBeTruthy();
    // the sample's "Main" button: ACC text on S2
    const texts = [...svg.querySelectorAll('g[data-i] text')];
    const main = texts.find((t) => t.textContent === 'Main')!;
    main.dispatchEvent(new MouseEvent('click', { bubbles: true, composed: true }));
    await settle(el);
    expect([...root.querySelectorAll('.tk.hl')].map((r) => r.getAttribute('data-token')).sort()).toEqual(['acc', 's2']);
    const row = root.querySelector('[data-token="bad"]')!;
    row.dispatchEvent(new MouseEvent('mouseenter'));
    await settle(el);
    expect(root.querySelectorAll('svg.pv rect[stroke="#ff2bd6"]').length).toBeGreaterThan(0);
  });
});
