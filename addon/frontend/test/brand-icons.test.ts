import { describe, expect, it } from 'vitest';
import { parseShapes, shapesFor } from '../src/lib/icon-shapes';
import { suggestIcons } from '../src/lib/icon-suggest';
import { STOCK_ICONS } from '../src/lib/stock-icons';

describe('brand logos', () => {
  it('filled paths are flagged, stroked ones are not', () => {
    expect(parseShapes('<path d="M0 0h1" fill="#000" stroke="none"/>')[0]).toMatchObject({ filled: true });
    expect(parseShapes('<path d="M0 0h1"/>')[0]).not.toHaveProperty('filled');
  });
  it('plex and youtube are stock icons 65 and 66, drawn filled', async () => {
    await (await import('../src/lib/icon-shapes')).brandsLoaded;
    const by = (n: string) => STOCK_ICONS.find((i) => i.name === n);
    expect(by('plex')?.id).toBe(65);
    expect(by('youtube')?.id).toBe(66);
    expect(shapesFor(65)[0]).toMatchObject({ filled: true });
    expect(shapesFor(66)[0]).toMatchObject({ filled: true });
  });
  it('labels and commands named Plex / YouTube suggest the logos', () => {
    expect(suggestIcons('', 'Plex')[0]?.icon.name).toBe('plex');
    expect(suggestIcons('YouTube')[0]?.icon.name).toBe('youtube');
    expect(suggestIcons('', 'You Tube')[0]?.icon.name).toBe('youtube');
  });
});

describe('brand card colours', () => {
  it('every brand icon carries a hex card colour and filled shapes', async () => {
    await (await import('../src/lib/icon-shapes')).brandsLoaded;
    const brands = STOCK_ICONS.filter((i) => i.brand);
    expect(brands.length).toBe(27);
    for (const i of brands) {
      expect(i.brand).toMatch(/^#[0-9A-F]{6}$/);
      expect(shapesFor(i.id).every((s) => s.tag === 'path' && s.filled)).toBe(true);
    }
    expect(STOCK_ICONS.find((i) => i.name === 'youtube')?.brand).toBe('#FF0000');
  });
});

describe('lazy brand markup', () => {
  it('brandsLoaded resolves and the brand shapes are there', async () => {
    const { brandsLoaded } = await import('../src/lib/icon-shapes');
    await brandsLoaded;
    expect(shapesFor(65).length).toBeGreaterThan(0);
  });
});
