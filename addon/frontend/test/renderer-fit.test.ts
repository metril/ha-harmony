import { describe, expect, it } from 'vitest';
import { layoutPage, type Prim } from '../src/lib/page-renderer';
import type { V2Page } from '../src/lib/api';

type W = V2Page['widgets'][number];
const wd = (o: Partial<W>): W => ({ kind: 'button', id: 1, x: 2, y: 0, w: 2, h: 1, label: 'Shield TV YouTube Premium Music Player Edition', role: 'default', shape: 'card', icon_only: false, icon: 0, bind: null, ...o }) as W;
const page = (w: W, cols = 4, rows = 5): V2Page => ({ version: 2, id: 0, cols, rows, gen: 0, total: 1, kind: 'normal', back: false, seq: null, act: 0, title: 'T', widgets: [w] });
const texts = (p: V2Page, flag = false) => layoutPage(p, { flagOverflow: flag }).prims.filter((q): q is Extract<Prim, { t: 'text' }> => q.t === 'text');
const dashed = (p: V2Page) => layoutPage(p, { flagOverflow: true }).prims.some((q) => q.t === 'rect' && q.dash);

describe('page renderer: real clipping and overflow flags', () => {
  it("cuts a label with three periods at the remote's pixel budget", () => {
    // a compact card wraps to two lines; the second is cut with three periods
    const shown = texts(page(wd({}))).map((t) => t.s);
    expect(shown.some((t) => t.endsWith('...'))).toBe(true);
    expect(shown.some((t) => t === 'Shield TV')).toBe(true);
  });
  it('draws a dashed red outline and a minus-N-px badge only when asked', () => {
    const p = page(wd({}));
    expect(layoutPage(p).prims.some((q) => q.t === 'rect' && q.dash)).toBe(false);
    expect(layoutPage(p, { flagOverflow: true }).prims.some((q) => q.t === 'rect' && q.dash && q.stroke === '#ff3b30')).toBe(true);
    expect(texts(p, true).some((t) => /^−\d+ px$/.test(t.s))).toBe(true);
  });
  it('flags nothing for a label that fits, or an icon-only one', () => {
    expect(dashed(page(wd({ label: 'Plex' })))).toBe(false);
    expect(dashed(page(wd({ icon: 24, icon_only: true })))).toBe(false);
  });
  it('sets a one or two character pill label in b20', () => {
    expect(texts(page(wd({ x: 0, w: 1, label: '7', shape: 'pill' }), 3, 4)).find((t) => t.s === '7')?.size).toBe(20);
  });
});
