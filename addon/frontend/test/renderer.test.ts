import { describe, expect, it } from 'vitest';
import { render } from 'lit';
import vectors from '../../../tests/vectors/pages.json';
import keymap from './fixtures/keymap.json';
import { cellAt, hitWidget, layoutPage, pageSvg, widgetRect, type Prim } from '../src/lib/page-renderer';
import { BODY, DPAD, KEYS, PHYSICAL_KEYS, SCREEN, keyByName, keyTitle } from '../src/lib/remote-layout';
import type { V2Page } from '../src/lib/api';
import { css as hexOf } from '../src/lib/themes';
import { DEFAULT_THEME } from '../src/lib/theme-model';

const PAL = { bg: hexOf(DEFAULT_THEME.tokens.bg), acc: hexOf(DEFAULT_THEME.tokens.acc), s2: hexOf(DEFAULT_THEME.tokens.s2) };

type Vec = { name: string; dict: V2Page };
const v2 = vectors.valid_v2 as unknown as Vec[];
const v1 = vectors.valid as unknown as Vec[];

describe('page renderer: shared golden vectors', () => {
  for (const v of [...v1, ...v2]) {
    it(`draws ${v.name}`, () => {
      const scene = layoutPage(v.dict);
      expect(scene.prims[0]).toMatchObject({ t: 'rect', w: 240, h: 320, fill: PAL.bg });
      // every drawn primitive stays on the 240x320 display
      for (const q of scene.prims) {
        if (q.t === 'rect') {
          expect(q.x).toBeGreaterThanOrEqual(0);
          expect(q.x + q.w).toBeLessThanOrEqual(240.01);
          expect(q.y + q.h).toBeLessThanOrEqual(320.01);
        }
      }
      expect(scene).toMatchSnapshot();
    });
    it(`${v.name} renders to SVG without throwing`, () => {
      const host = document.createElement('div');
      render(pageSvg(v.dict, {}, 2), host);
      const svg = host.querySelector('svg');
      expect(svg?.getAttribute('width')).toBe('480');
      expect(host.querySelectorAll('text').length).toBeGreaterThanOrEqual(1);
    });
  }

  it('widgets never overlap on screen and sit below the bar', () => {
    for (const v of [...v1, ...v2]) {
      const rects = v.dict.widgets.map((w) => widgetRect(v.dict, w));
      rects.forEach((a, i) => {
        expect(a.y1).toBeGreaterThanOrEqual(32);
        expect(a.x2).toBeLessThan(240);
        expect(a.y2).toBeLessThan(320);
        rects.slice(i + 1).forEach((b) => expect(a.x1 > b.x2 || b.x1 > a.x2 || a.y1 > b.y2 || b.y1 > a.y2, `${v.name} ${i}`).toBe(true));
      });
    }
  });
});

describe('page renderer: v2 looks', () => {
  const full = v2.find((v) => v.name === 'v2_all_options')!.dict;
  const texts = (s: Prim[]) => s.filter((p): p is Extract<Prim, { t: 'text' }> => p.t === 'text').map((p) => p.s);
  it('shows the title, paged idx/cnt and the back chevron', () => {
    const s = layoutPage(full).prims;
    expect(texts(s)).toContain('Watch TV é');
    expect(texts(s)).toContain('2/3');
    expect(s.some((p) => p.t === 'path' && p.d.startsWith('M16 10'))).toBe(true);
  });
  it('uses Harmony when there is no title, and promotes a title label only for v1', () => {
    expect(texts(layoutPage(v2.find((v) => v.name === 'v2_minimal')!.dict).prims)).toContain('Harmony');
    const mixed = v1.find((v) => v.name === 'mixed')!.dict;
    const first = mixed.widgets[0]!;
    if (first.kind === 'label' && first.x === 0 && first.y === 0 && first.w === mixed.cols) expect(texts(layoutPage(mixed).prims)).toContain(first.label || 'Harmony');
  });
  it('draws roles, icons and the active highlight', () => {
    const prims = layoutPage(full, { activeAct: 0 }).prims;
    expect(prims.some((p) => p.t === 'icon')).toBe(true);
    // the running activity: ACC fill; otherwise the primary role is ACC text on an S2 surface (remote/app/ui.c)
    expect(prims.find((p) => p.t === 'rect' && p.wi === 0)).toMatchObject({ fill: PAL.acc });
    const idle = layoutPage(full, { activeAct: null }).prims.find((p) => p.t === 'rect' && p.wi === 0);
    expect(idle).toMatchObject({ fill: PAL.s2 });
  });
  it('align centre moves a row button\'s icon + label group to the middle; default stays left', () => {
    const row = { kind: 'button', id: 1, x: 0, y: 0, w: 2, h: 1, label: 'Go', role: 'default', shape: 'row', icon_only: false, icon: 1, bind: null } as V2Page['widgets'][number];
    const at = (w: V2Page['widgets'][number]) => layoutPage({ ...full, widgets: [w] }).prims.find((p) => p.t === 'icon' && p.wi === 0) as Extract<Prim, { t: 'icon' }>;
    expect(at(row).x).toBeLessThan(30);
    expect(at({ ...row, align_center: true }).x).toBeGreaterThan(36);
  });
  it('trailing draws a right-aligned chevron and centres in the width left of it', () => {
    const row = { kind: 'button', id: 1, x: 0, y: 0, w: 2, h: 1, label: 'Go', role: 'default', shape: 'row', icon_only: false, icon: 1, bind: null } as V2Page['widgets'][number];
    const ic = (w: V2Page['widgets'][number], id: number) => layoutPage({ ...full, widgets: [w] }).prims.find((p) => p.t === 'icon' && p.wi === 0 && p.id === id) as Extract<Prim, { t: 'icon' }>;
    expect(ic(row, 8)).toBeUndefined();
    const t = { ...row, trailing: true };
    const rect = layoutPage({ ...full, widgets: [t] }).prims.find((p) => p.t === 'rect' && p.wi === 0) as Extract<Prim, { t: 'rect' }>;
    expect(ic(t, 8).x + 24).toBeCloseTo(rect.x + rect.w - 8 - 2, 0);
    expect(ic({ ...t, align_center: true }, 1).x).toBeLessThan(ic({ ...row, align_center: true }, 1).x);
    const lab = (w: V2Page['widgets'][number]) => layoutPage({ ...full, widgets: [w] }).prims.find((p) => p.t === 'text' && p.wi === 0) as Extract<Prim, { t: 'text' }>;
    const nl = { ...row, icon: 0, align_center: true };
    expect(lab({ ...nl, trailing: true }).x).toBeLessThan(lab(nl).x);
  });
  it('tiles show on, off and unavailable', () => {
    const page: V2Page = { ...full, back: false, seq: null, widgets: [0, 1, 2].map((i) => ({ kind: 'tile', id: i, x: 0, y: i * 2, w: 4, h: 2, label: 'T' + i, role: 'default', shape: 'card', icon_only: false, icon: 0, bind: null })) };
    const prims = layoutPage(page).prims;
    expect(texts(prims)).toEqual(expect.arrayContaining(['On', 'Off', 'n/a']));
  });
  it('hit-tests like the remote', () => {
    const p = v2.find((v) => v.name === 'v2_title')!.dict;
    const r = widgetRect(p, p.widgets[0]!);
    expect(hitWidget(p, r.x1 + 2, r.y1 + 2)).toBe(0);
    expect(hitWidget(p, 200, 300)).toBe(-1);
    expect(cellAt(p, 5, 10)).toBeNull();
    expect(cellAt(p, 239, 319)).toEqual({ x: 3, y: 5 });
  });
});

describe('remote layout', () => {
  const names = keymap.default.map((r) => r.key);
  it('has geometry for every key the backend knows, and nothing else', () => {
    expect(KEYS.map((k) => k.name).sort()).toEqual([...names, 'home', 'favorites'].filter((n, i, a) => a.indexOf(n) === i).sort());
    expect(KEYS).toHaveLength(35);
  });
  it('keeps keys inside the body, off the screen, and apart', () => {
    for (const k of KEYS) {
      expect(k.x).toBeGreaterThan(BODY.x);
      expect(k.x + k.w).toBeLessThan(BODY.x + BODY.w);
      if (k.name !== 'power_off_activity') expect(k.y).toBeGreaterThanOrEqual(SCREEN.y + SCREEN.h);
      expect(k.y + k.h).toBeLessThan(BODY.y + BODY.h);
    }
    const hit = (a: (typeof KEYS)[number], b: (typeof KEYS)[number]) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    // the D-pad cluster's outlines interlock, so their bounding boxes overlap by design; compare the rest
    const plain = KEYS.filter((k) => k.shape !== 'path' && k.shape !== 'circle');
    plain.forEach((a, i) => plain.slice(i + 1).forEach((b) => expect(hit(a, b), `${a.name}/${b.name}`).toBe(false)));
    expect(keyByName.get('select')!.shape).toBe('circle');
  });
  it('draws the real arrangement: Off above the screen, labelled strips, Exit left of Menu, rockers, 2x2 home block', () => {
    const g = (n: string) => keyByName.get(n)!;
    expect(g('power_off_activity').y + g('power_off_activity').h).toBeLessThanOrEqual(SCREEN.y);
    expect([g('home').label, g('favorites').label]).toEqual(['ACTIVITIES', 'DEVICES']);
    expect(g('home').x).toBeLessThan(g('favorites').x);
    expect(g('exit').x).toBeLessThan(g('menu').x);
    expect(g('play').y).toBeLessThan(g('pause').y);
    expect(g('rewind').x).toBeLessThan(g('play').x);
    expect(g('stop').x).toBeGreaterThan(g('play').x);
    expect(g('volume_up').x).toBeLessThan(g('left').x);
    expect(g('channel_up').x).toBeGreaterThan(g('right').x);
    expect(g('volume_up').y).toBeLessThan(g('volume_down').y);
    expect(g('volume_up').x).toBe(g('volume_down').x);
    expect(g('volume_mute').x).toBeLessThan(g(PHYSICAL_KEYS.return).x);
    expect(g('dvr').x).toBeLessThan(g('guide').x);
    expect(g('guide').x).toBeLessThan(g('info').x);
    expect(['red', 'green', 'yellow', 'blue'].map((n) => g(n).x)).toEqual(['red', 'green', 'yellow', 'blue'].map((n) => g(n).x).sort((a, b) => a - b));
    const { lightUpper: lu, lightLower: ll, socketUpper: su, socketLower: sl } = PHYSICAL_KEYS;
    expect(g(lu).y).toBeLessThan(g(ll).y);
    expect(g(su).y).toBeLessThan(g(sl).y);
    expect(g(lu).x).toBeLessThan(g('home_control_plus').x);
    expect(g('home_control_plus').x).toBeLessThan(g(su).x);
    expect(g('home_control_plus').y).toBeLessThan(g('home_control_minus').y);
  });
  it('keeps the photo proportions: tall body, Off well below the top, empty grip at the bottom, 240x320 page', () => {
    const g = (n: string) => keyByName.get(n)!;
    const fy = (k: { y: number; h: number }) => (k.y + k.h / 2 - BODY.y) / BODY.h;
    expect(BODY.h / BODY.w).toBeGreaterThan(3.55);
    expect(BODY.h / BODY.w).toBeLessThan(3.8);
    expect(fy(g('power_off_activity'))).toBeGreaterThan(0.04);
    expect(Math.max(...KEYS.map((k) => (k.y + k.h - BODY.y) / BODY.h))).toBeLessThan(0.9);
    expect(fy({ y: DPAD.y, h: 0 })).toBeCloseTo(0.633, 2);
    expect([SCREEN.w, SCREEN.h]).toEqual([240, 320]);
  });
  it('names keys with their function and what is printed on them', () => {
    expect(keyTitle('home')).toBe('Home (ACTIVITIES)');
    expect(keyTitle('volume_up')).toBe('Volume Up (Vol +)');
    expect(keyTitle('play')).toBe('Play');
    expect(keyTitle('prev_channel')).toBe('Previous Channel (Back)');
  });
});

describe('colour and home-control keys', () => {
  it('colour keys are wide, evenly spaced and span the home-control block', () => {
    const c = ['red', 'green', 'yellow', 'blue'].map((n) => keyByName.get(n)!);
    c.forEach((k) => expect(k.w / k.h).toBeGreaterThan(2));
    const gaps = c.slice(1).map((k, i) => k.x - (c[i]!.x + c[i]!.w));
    gaps.forEach((g) => expect(g).toBeCloseTo(gaps[0]!, 0));
    const l = keyByName.get(PHYSICAL_KEYS.lightUpper)!, r = keyByName.get(PHYSICAL_KEYS.socketUpper)!;
    expect(c[0]!.x).toBeCloseTo(l.x, 0);
    expect(c[3]!.x + c[3]!.w).toBeCloseTo(r.x + r.w, 0);
  });
});
