import { describe, expect, it } from 'vitest';
import fw from './fixtures/firmware-fills.json';
import type { V2Page, V2Widget } from '../src/lib/api';
import { layoutPage, type Prim } from '../src/lib/page-renderer';
import { activeTheme, contrastInfo, deriveSwatches, labelColor, presetTheme, resolveColour, themeNamed, widgetColors } from '../src/lib/theme-model';
import { css, PALETTE_NAMES, THEMES } from '../src/lib/themes';
import { from565, snap565 } from '../src/lib/theme-math';

const w = (id: number, kind: V2Widget['kind'], x: number, y: number, label: string, extra: Partial<V2Widget> = {}): V2Widget => ({
  kind, id, x, y, w: kind === 'label' && id === 0 ? 2 : 1, h: 1, label, role: 'default', shape: 'card', icon_only: false, icon: 0, bind: null, ...extra,
});
const P = (n: number) => PALETTE_NAMES[n - 1] as string;
/** The firmware's "Colours" reference screen (tests/c/colour_screens.h) */
function colourPage(): V2Page {
  const ws: V2Widget[] = [
    w(0, 'label', 0, 0, 'Blue label', { color: P(9) }), w(1, 'label', 2, 0, 'Yellow', { color: P(4) }),
    w(2, 'button', 0, 1, 'Red', { color: P(1) }), w(3, 'button', 1, 1, 'Green', { color: P(6) }), w(4, 'button', 2, 1, 'Blue', { color: P(9) }),
    w(5, 'button', 0, 2, 'Sun', { color: snapHex(0xffe0) }), w(6, 'button', 1, 2, 'Ink', { color: snapHex(0x0841) }), w(7, 'button', 2, 2, 'Snow', { color: snapHex(0xef7d) }),
    w(8, 'button', 0, 3, 'Active', { color: P(10), bind: 0 }), w(9, 'button', 1, 3, 'Pressed', { color: P(11) }), w(10, 'button', 2, 3, 'Role', { role: 'primary' }),
    w(11, 'tile', 0, 4, 'Lamp', { on_color: P(11) }), w(12, 'tile', 1, 4, 'Plain'), w(13, 'tile', 2, 4, 'Fan', { color: P(7) }),
    w(14, 'tile', 0, 5, 'Temp', { color: P(3) }), w(15, 'tile', 1, 5, 'Door', { color: P(1) }), w(16, 'tile', 2, 5, 'Light', { on_color: P(12), color: P(7) }),
  ];
  return { version: 2, id: 0, cols: 3, rows: 6, gen: 0, total: 1, kind: 'normal', back: false, seq: null, act: 0, title: 'Colours', widgets: ws };
}
const snapHex = (v565: number) => css(from565(v565)).toUpperCase();
const TILES = { 11: 'on', 12: 'on', 13: 'off', 14: 'value', 15: 'unavailable', 16: 'on' } as const;
const tiles = Object.fromEntries(Object.entries(TILES).map(([k, state]) => [k, { state, text: state }]));
const fillOf = (prims: Prim[], wi: number): number => {
  const r = prims.find((q) => q.t === 'rect' && q.wi === wi && q.w > 20) ?? prims.find((q) => q.t === 'circle' && q.wi === wi);
  const f = (r as { fill: string }).fill;
  return parseInt(f.slice(1), 16);
};
const sameFill = (a: number, b: number) => a === b;

describe('the preview draws what the firmware draws (pixels of the reference screens)', () => {
  fw.themes.forEach((key, ti) => {
    it(`widget fills in ${key}`, () => {
      const page = colourPage();
      const prims = layoutPage(page, { activeAct: 0, tiles, theme: presetTheme(key) }).prims;
      for (const [id, fills] of Object.entries(fw.fills)) {
        const wi = Number(id);
        // a rect with a 3 px active border / 1 px outline keeps its fill
        expect(sameFill(fillOf(prims, wi), fills[ti] as number), `${key} widget ${id}: ${css(fillOf(prims, wi))} vs ${css(fills[ti] as number)}`).toBe(true);
      }
    });
    it(`active border in ${key}`, () => {
      const prims = layoutPage(colourPage(), { activeAct: 0, tiles, theme: presetTheme(key) }).prims;
      const r = prims.find((q) => q.t === 'rect' && q.wi === 8) as Extract<Prim, { t: 'rect' }>;
      expect(r.sw).toBe(3);
      expect(parseInt((r.stroke as string).slice(1), 16)).toBe((fw.borders as Record<string, number[]>)['8']?.[ti]);
    });
  });
});

describe('widget colour rules', () => {
  const th = presetTheme('default');
  const T = th.tokens;
  it('role defaults, active, nav', () => {
    expect(widgetColors({ kind: 'button', role: 'default' }, {}, th)).toMatchObject({ fill: T.s2, ink: T.tx });
    expect(widgetColors({ kind: 'button', role: 'primary' }, {}, th)).toMatchObject({ fill: T.s2, ink: T.acc });
    expect(widgetColors({ kind: 'button', role: 'danger' }, {}, th)).toMatchObject({ fill: T.s2, ink: T.bad });
    expect(widgetColors({ kind: 'button', role: 'secondary' }, {}, th)).toMatchObject({ fill: T.s1, ink: T.tx2 });
    expect(widgetColors({ kind: 'button', role: 'default' }, { active: true }, th)).toMatchObject({ fill: T.acc, ink: T.onacc });
  });
  it('a coloured active button keeps its fill and gets a border (TX when the fill is the accent)', () => {
    expect(widgetColors({ kind: 'button', role: 'default', color: 'red' }, { active: true }, th).border).toBe(T.acc);
    expect(widgetColors({ kind: 'button', role: 'default', color: 'teal' }, { active: true }, th)).toMatchObject({ fill: T.acc, border: T.tx });
  });
  it('tile state wins over the fill; on_color only while on', () => {
    const t = { kind: 'tile', role: 'default', color: 'blue', on_color: 'pink' } as const;
    expect(widgetColors(t, { state: 'on' }, th).fill).toBe(th.swatches[11]?.fill);
    expect(widgetColors(t, { state: 'off' }, th).fill).toBe(th.swatches[8]?.fill);
    expect(widgetColors(t, { state: 'unavailable' }, th)).toMatchObject({ fill: T.s1, ink: T.tx3 });
    expect(widgetColors({ kind: 'tile', role: 'default', color: 'blue' }, { state: 'on' }, th)).toMatchObject({ fill: T.warm, ink: T.onwarm });
    expect(widgetColors({ kind: 'tile', role: 'default', color: 'blue' }, { state: 'off', stale: true }, th)).toMatchObject({ fill: T.s1, ink: T.tx3 });
  });
  it('free colours snap, get a derived ink and an automatic outline', () => {
    const r = resolveColour('#FFFF00', th)!;
    expect(r.fill).toBe(snap565(0xffff00));
    expect(r.ink).toBe(T.bg);
    expect(widgetColors({ kind: 'button', role: 'default', color: '#080808' }, {}, th).outline).toBe(true);
    expect(widgetColors({ kind: 'button', role: 'default', color: 'red' }, {}, th).outline).toBe(false);
  });
  it('a coloured label falls back to TX when it is unreadable on the ground', () => {
    expect(labelColor('#101820', th)).toBe(T.tx);
    expect(labelColor('blue', th)).toBe(th.swatches[8]?.fill);
    expect(labelColor(undefined, th)).toBeNull();
  });
  it('derived ink and pressed equal the preset tables (deriveSwatches is what user themes use)', () => {
    for (const t of THEMES) {
      const d = deriveSwatches(t.tokens, t.palette.map((x) => x.fill));
      expect(d.map((x) => [x.ink, x.pressed])).toEqual(t.palette.map((x) => [x.ink, x.pressed]));
    }
  });
  it('contrast read-out: ok / warn with a nearest readable fix', () => {
    expect(contrastInfo('red', th)?.ok).toBe(true);
    const mid = contrastInfo('#777777', th)!;
    expect(mid.ratio_x100).toBeLessThan(450);
    expect(mid.ok).toBe(false);
    expect(mid.fix).toMatch(/^#[0-9A-F]{6}$/);
    expect(contrastInfo(mid.fix as string, th)?.ok).toBe(true);
  });
});

describe('the config selects the theme', () => {
  it('default, preset, user theme with extends, broken falls back', () => {
    expect(activeTheme({}).theme.name).toBe('default');
    expect(activeTheme({ theme: 'daylight' }).theme.tokens.bg).toBe(0xe8ecf0);
    const cfg = { theme: 'dusk', themes: { dusk: { extends: 'midnight', tokens: { ACC: '#FF8A3D' }, palette: { '3': { fill: '#2E7D32' } } } } };
    const t = activeTheme(cfg).theme;
    expect(t.base).toBe('midnight');
    expect(t.tokens.acc).toBe(snap565(0xff8a3d));
    expect(t.tokens.bg).toBe(0x000000);
    expect(t.swatches[2]?.fill).toBe(snap565(0x2e7d32));
    expect(themeNamed({ themes: { a: { extends: 'a' } } }, 'a').error).toMatch(/circular|deeper/);
  });
  it('a theme-less config draws in the page theme id preset', () => {
    const p = { ...colourPage(), theme: 2 };
    expect(layoutPage(p).prims[0]).toMatchObject({ fill: css(0xe8ecf0) });
  });
});
