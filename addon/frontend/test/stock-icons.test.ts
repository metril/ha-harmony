import { describe, expect, it } from 'vitest';
import { GLYPH_BASE, STOCK_ICONS, glyphFor, stockIconById, stockIconByName, stockIconId } from '../src/lib/stock-icons';

describe('stock icons (shared with the compiler)', () => {
  it('has contiguous ids from 1 with markup and keywords', () => {
    expect(STOCK_ICONS.length).toBeGreaterThan(40);
    STOCK_ICONS.forEach((ic, i) => {
      expect(ic.id).toBe(i + 1);
      expect(ic.svg.length).toBeGreaterThan(0);
      expect(ic.keywords.length).toBeGreaterThan(0);
    });
  });

  it('looks icons up by id and name; unknown is 0', () => {
    expect(stockIconByName('tv')?.id).toBe(1);
    expect(stockIconById(1)?.name).toBe('tv');
    expect(stockIconId('power')).toBe(stockIconByName('power')?.id);
    expect(stockIconId('none')).toBe(0);
    expect(stockIconId('nope')).toBe(0);
    expect(glyphFor(1).codePointAt(0)).toBe(GLYPH_BASE + 1);
  });
});
