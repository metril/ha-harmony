import { describe, expect, it } from 'vitest';
import vec from '../../../tests/vectors/themes.json';
import {
  contrastX100, from565, ink, is565, meets, nearestPalette, needsOutline, pressed, resolveTheme, serialiseTheme,
  snap565, to565, validateTheme, WIRE_LEN, type Tokens, type UserTheme,
} from '../src/lib/theme-math';
import { css, CUSTOM_THEME_ID, LIN, PALETTE_NAMES, swatchOf, themeById, THEMES, TOKEN_NAMES } from '../src/lib/themes';

const hex = (u: Uint8Array): string => Array.from(u, (b) => b.toString(16).padStart(2, '0')).join('');
const presets = Object.fromEntries(THEMES.map((t) => [t.key, t]));

describe('themes.ts shape', () => {
  it('ids, order, palette', () => {
    expect(THEMES.map((t) => t.key)).toEqual(['default', 'midnight', 'daylight', 'high_contrast']);
    expect(new Set(THEMES.map((t) => t.id)).size).toBe(THEMES.length);
    expect(Math.max(...THEMES.map((t) => t.id))).toBeLessThan(CUSTOM_THEME_ID);
    expect(themeById(0)?.name).toBe('Cards');
    expect(PALETTE_NAMES).toHaveLength(12);
    for (const t of THEMES) {
      expect(Object.keys(t.tokens)).toEqual([...TOKEN_NAMES]);
      expect(t.palette.map((s) => s.name)).toEqual([...PALETTE_NAMES]);
      for (const c of [...Object.values(t.tokens), ...t.palette.flatMap((s) => [s.fill, s.ink, s.pressed])]) expect(is565(c)).toBe(true);
      expect(validateTheme(t.tokens, t.palette.map((s) => s.fill))).toEqual([]);
      expect(swatchOf(t, 1)?.name).toBe('red');
      expect(swatchOf(t, 0)).toBeUndefined();
    }
    expect(css(0x101820)).toBe('#101820');
  });
  it('the default preset is the theme.h palette', () => {
    expect(presets.default!.tokens.acc).toBe(0x20c8b0);
    expect(presets.default!.tokens.bg).toBe(0x101820);
  });
});

describe('shared vectors (bit-for-bit with Python and C)', () => {
  it('lin table', () => expect([...LIN]).toEqual(vec.lin));
  it('snap / contrast / derive / outline', () => {
    for (const [c, w] of vec.snap) expect(snap565(c!)).toBe(w);
    for (const [a, b, w] of vec.contrast) expect(contrastX100(a!, b!)).toBe(w);
    expect(vec.derive.length).toBeGreaterThanOrEqual(300);
    for (const [f, t, b, i, p] of vec.derive) {
      expect(ink(f!, t!, b!)).toBe(i);
      expect(pressed(f!, i!)).toBe(p);
    }
    for (const [f, b, w] of vec.outline) expect(needsOutline(f as number, b as number)).toBe(w);
  });
  it('nearest palette', () => {
    const fills = presets.default!.palette.map((s) => s.fill);
    for (const [c, w] of vec.nearest_palette.cases) expect(nearestPalette(c!, fills)).toBe(w);
  });
  it('validate with fixes', () => {
    for (const v of vec.validate) {
      expect(validateTheme(v.tokens as Tokens, v.fills)).toEqual(v.problems);
      for (const p of v.problems) expect(is565(p.fix)).toBe(true);
    }
  });
  it('wire form', () => {
    const d = presets.default!;
    const w = serialiseTheme(d.tokens, d.palette.map((s) => s.fill));
    expect(w).toHaveLength(WIRE_LEN);
    expect(hex(w).slice(0, 32)).toBe('0110c419262167320affffadb86bd126');
    for (const e of vec.wire) expect(hex(serialiseTheme(e.tokens as Tokens, e.fills))).toBe(e.hex);
    expect(from565(to565(0xf8fcf8))).toBe(0xf8fcf8);
    expect(meets(0xf8fcf8, 0, 9, 2)).toBe(true);
  });
});

describe('resolveTheme', () => {
  const users: Record<string, UserTheme> = {
    mine: { extends: 'midnight', tokens: { acc: '#ff0000' }, palette: { '1': { fill: '#123456' } } },
    child: { extends: 'mine', tokens: { tx: '#f0f0f0' } },
  };
  it('extends chains and snaps', () => {
    const r = resolveTheme('child', users, presets);
    expect(r.tokens.acc).toBe(0xf80000);
    expect(r.tokens.tx).toBe(0xf0f0f0);
    expect(r.tokens.bg).toBe(presets.midnight!.tokens.bg);
    expect(r.fills[0]).toBe(snap565(0x123456));
    expect(r.fills[1]).toBe(presets.midnight!.palette[1]!.fill);
    expect(resolveTheme('default', {}, presets).fills).toHaveLength(12);
  });
  it('errors', () => {
    const bad = (n: string, t: Record<string, UserTheme>, m: RegExp): void => expect(() => resolveTheme(n, t, presets)).toThrow(m);
    bad('midnight', { midnight: { extends: 'default' } }, /reuse a preset/);
    bad('x', { x: { extends: 'default', tokens: { nope: '#000000' } } }, /unknown token/);
    bad('x', { x: { extends: 'default', tokens: { bg: '#12345' } } }, /bad colour/);
    bad('x', { x: { extends: 'ghost' } }, /unknown theme/);
    bad('x', { x: {} }, /extends must name/);
    bad('x', { x: { extends: 'y' }, y: { extends: 'x' } }, /circular/);
    bad('x', { x: { extends: 'default', palette: { '13': { fill: '#000000' } } } }, /palette index/);
    const chain: Record<string, UserTheme> = { t5: { extends: 'default' } };
    for (let i = 0; i < 5; i++) chain[`t${i}`] = { extends: `t${i + 1}` };
    bad('t0', chain, /deeper than 4/);
  });
});
