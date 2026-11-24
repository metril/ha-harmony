// Draws a page-format-v2 model (harmony_core.page_codec.page_to_dict, the shared contract with the C
// remote) the way the remote does: the config's theme (themes.ts, colour rules of theme-model.ts = remote/app/ui.c), 32 px top bar, cards/rows/pills/monograms,
// roles, stock icons, tiles with state, active-activity highlight, paged "idx/cnt", back chevron.
// Geometry mirrors remote/app/ui.c (cells use ceil edges, 8 px margins, 8 px gaps).
// `layoutPage` returns plain primitives (snapshot-tested against tests/vectors/pages.json);
// `pageGroup` turns them into SVG.
import { svg, type TemplateResult } from 'lit';
import { iconInner } from './icon-shapes';
import type { V2Page, V2Widget } from './api';
import { THEMES, css as hex } from './themes';
import { clip, fitLabel, lineHeight, textPx, type Font } from './text-fit';
import { DEFAULT_THEME, labelColor, presetTheme, widgetColors, type RenderTheme, type TileState } from './theme-model';

export const SCREEN_W = 240;
export const SCREEN_H = 320;
export const BAR_H = 32;
const MARGIN = 8;
const GAP = 8;

/** The preset a page's theme id names (7, the config's own theme, needs `opts.theme`). */
const themeOfPage = (p: V2Page): RenderTheme => {
  const t = THEMES.find((x) => x.id === (p.theme ?? 0));
  return t ? presetTheme(t.key) : DEFAULT_THEME;
};

export type Prim =
  | { t: 'rect'; x: number; y: number; w: number; h: number; r: number; fill: string; stroke?: string; sw?: number; dash?: boolean; wi?: number }
  | { t: 'circle'; x: number; y: number; r: number; fill: string; stroke?: string; sw?: number; wi?: number }
  | { t: 'text'; x: number; y: number; s: string; size: number; fill: string; anchor: 'start' | 'middle' | 'end'; weight?: number; wi?: number }
  | { t: 'icon'; id: number; x: number; y: number; size: number; color: string; wi?: number }
  | { t: 'path'; d: string; stroke: string; fill?: string; sw: number; wi?: number };

export interface TileSample {
  state: TileState;
  text: string;
  /** link lost: the remote greys every tile that is not on */
  stale?: boolean;
}
export interface RenderOpts {
  /** activity index that is running; widgets bound to it show the active highlight */
  activeAct?: number | null;
  /** sample tile state by widget id; default cycles on / off / unavailable */
  tiles?: Record<number, TileSample>;
  /** hide the top bar status icons (thumbnails) */
  noStatus?: boolean;
  /** the config's active theme (`activeTheme(config).theme`); default: the preset the page names */
  theme?: RenderTheme;
  /** back stack depth of the page (the remote's chevron shows above 1); default: Home 1, every other page 2 */
  depth?: number;
  /** studio: outline every label the remote would cut, with a -N px badge */
  flagOverflow?: boolean; // the Studio no longer sets this (the amber issue dot replaces it); kept for renderer-fit tests and re-enable
}
export interface Scene {
  w: number;
  h: number;
  prims: Prim[];
}
export interface Rect {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

const edge = (i: number, n: number, total: number) => Math.ceil((i * total) / n);

/** v1 pages promote a full-width row-0 label to the title (and shift rows up); v2 never does. */
export function titleIndex(p: V2Page): number {
  if (p.version !== 1) return -1;
  return p.widgets.findIndex((w) => w.kind === 'label' && w.x === 0 && w.y === 0 && w.w === p.cols && w.h === 1);
}
export const rowShift = (p: V2Page): number => (p.rows > 1 && titleIndex(p) >= 0 ? 1 : 0);

const isFixed = (p: V2Page): boolean => !!(p as { fixed?: boolean }).fixed;
export const FIXED_MAX = 276;
const FIXED_TOP = 8;
const FIXED_SPANS = [12, 6, 4, 3];
type BoxWidget = Pick<V2Widget, 'x' | 'y' | 'w' | 'h'> & Partial<Pick<V2Widget, 'kind' | 'shape'>> & { compact?: boolean };

/** FIXED layout class of a widget: pixel height and whether it is row-class; null when it cannot be placed (hp_page_box). */
export function fixedClass(w: BoxWidget): { h: number; row: boolean } | null {
  const sp = w.w;
  const kind = w.kind ?? 'button';
  const shape = w.shape ?? 'card';
  if (w.h !== 1 || !FIXED_SPANS.includes(sp) || w.x % sp) return null;
  if (w.compact && (kind !== 'button' || shape !== 'card' || sp !== 6)) return null;
  if (kind === 'label' || shape === 'row') return { h: 44, row: true };
  if (kind === 'tile') return { h: 64, row: false };
  if (shape === 'pill') return { h: 52, row: false };
  return { h: sp >= 6 ? (w.compact ? 56 : 80) : 52, row: false };
}
/** Stack offsets (from content y 8) per non-empty grid row, and the stacked height; null if a widget is unplaceable. */
export function fixedRows(p: V2Page): { tops: Map<number, number>; total: number } | null {
  const tops = new Map<number, number>();
  let cur = 0;
  let prevList: boolean | null = null;
  for (let y = 0; y < p.rows; y++) {
    const cls: { h: number; row: boolean }[] = [];
    for (const w of p.widgets) {
      if (w.y !== y) continue;
      const c = fixedClass(w as BoxWidget);
      if (!c) return null;
      cls.push(c);
    }
    if (!cls.length) continue;
    const list = cls.every((c) => c.row);
    if (prevList !== null) cur += prevList && list ? 2 : 8;
    tops.set(y, cur);
    cur += Math.max(...cls.map((c) => c.h));
    prevList = list;
  }
  return { tops, total: cur };
}

/** The widget's box in screen pixels, before the inset (hp_page_box in C, page_box in Python). */
export function widgetBox(p: V2Page, w: BoxWidget): Rect {
  if (isFixed(p)) {
    const c = fixedClass(w);
    const top = fixedRows(p)?.tops.get(w.y);
    if (c && top !== undefined) {
      const n = 12 / w.w;
      const wpx = Math.floor((224 - (n - 1) * 8) / n);
      const x1 = 8 + (w.x / w.w) * (wpx + 8);
      const y1 = BAR_H + FIXED_TOP + top;
      return { x1, y1, x2: x1 + wpx - 1, y2: y1 + c.h - 1 };
    }
  }
  const sh = rowShift(p);
  const rows = p.rows - sh;
  return {
    x1: edge(w.x, p.cols, SCREEN_W),
    x2: edge(w.x + w.w, p.cols, SCREEN_W) - 1,
    y1: BAR_H + edge(w.y - sh, rows, SCREEN_H - BAR_H),
    y2: BAR_H + edge(w.y + w.h - sh, rows, SCREEN_H - BAR_H) - 1,
  };
}
function inset(lo: number, hi: number, a: number, b: number): [number, number] {
  while (hi - lo + 1 - a - b < 12 && (a || b)) {
    if (a) a--;
    if (b) b--;
  }
  return [lo + a, hi - b];
}
/** The widget's drawn rectangle (what ra_ui_widget_rect gives the C remote). */
export function widgetRect(p: V2Page, w: BoxWidget): Rect {
  const b = widgetBox(p, w);
  if (isFixed(p)) return b; // FIXED boxes are the drawn rectangle: the gaps sit between widgets
  const [x1, x2] = inset(b.x1, b.x2, b.x1 === 0 ? MARGIN : GAP / 2, b.x2 === SCREEN_W - 1 ? MARGIN : GAP / 2);
  const [y1, y2] = inset(b.y1, b.y2, b.y1 === BAR_H ? MARGIN : GAP / 2, b.y2 === SCREEN_H - 1 ? MARGIN : GAP / 2);
  return { x1, y1, x2, y2 };
}
/** Grid cell under a screen pixel (page cells, with the v1 title row shift). */
export function cellAt(p: V2Page, px: number, py: number): { x: number; y: number } | null {
  if (px < 0 || py < BAR_H || px >= SCREEN_W || py >= SCREEN_H) return null;
  const sh = rowShift(p);
  return { x: Math.floor((px * p.cols) / SCREEN_W), y: Math.floor(((py - BAR_H) * (p.rows - sh)) / (SCREEN_H - BAR_H)) + sh };
}
/** Widget index under a screen pixel, or -1. */
export function hitWidget(p: V2Page, px: number, py: number): number {
  if (isFixed(p)) {
    if (px < 0 || py < BAR_H || px >= SCREEN_W || py >= SCREEN_H) return -1;
    return p.widgets.findIndex((w) => {
      const r = widgetBox(p, w as BoxWidget);
      return px >= r.x1 && px <= r.x2 && py >= r.y1 && py <= r.y2;
    });
  }
  const c = cellAt(p, px, py);
  if (!c) return -1;
  return p.widgets.findIndex((w) => c.x >= w.x && c.x < w.x + w.w && c.y >= w.y && c.y < w.y + w.h);
}

const FONT_SIZE: Record<Font, number> = { b13: 13, m16: 16, b18: 18, b20: 20, r12: 12 };
const FONT_WEIGHT: Record<Font, number> = { b13: 700, m16: 500, b18: 700, b20: 700, r12: 400 };
/** `s` as the remote draws it in `font` within `px`: cut with LVGL's three-period dots, never "…". */
const fit = (s: string, font: Font, px: number): string => clip(s, font, Math.max(0, px)).text;
const DANGER = '#ff3b30';

export function sampleTile(p: V2Page, w: V2Widget, opts: RenderOpts): TileSample {
  const given = opts.tiles?.[w.id];
  if (given) return given;
  const n = p.widgets.filter((x) => x.kind === 'tile' && x.id <= w.id).length - 1;
  return [
    { state: 'on', text: 'On' },
    { state: 'off', text: 'Off' },
    { state: 'unavailable', text: 'n/a' },
  ][n % 3] as TileSample;
}

function barPrims(p: V2Page, o: RenderOpts, T: RenderTheme, out: Prim[]): void {
  const k = T.tokens;
  const tIdx = titleIndex(p);
  const title = p.version === 1 ? (tIdx >= 0 ? (p.widgets[tIdx] as V2Widget).label : '') || 'Harmony' : p.title || 'Harmony';
  // chevron when the back stack is deeper than Home (remote/app/ui.c bar_build); the Studio has no stack: any page but Home counts as reached
  const depth = o.depth ?? (p.kind === 'home' ? 1 : 2);
  const back = p.version === 2 && depth > 1;
  let tx = 12;
  if (back) {
    out.push({ t: 'icon', id: 9, x: 4, y: 4, size: 24, color: hex(k.tx2) });
    tx = 38;
  }
  const paged = p.seq ? `${p.seq.idx}/${p.seq.cnt}` : '';
  const seqRight = o.noStatus ? 228 : 142;
  const sw = paged ? textPx(paged, 'r12') + 2 : 0;
  const room = paged ? seqRight - sw - 6 - tx : (o.noStatus ? 228 : 140) - tx;
  out.push({ t: 'text', x: tx, y: 22, s: fit(title, 'b18', room), size: 18, fill: hex(k.tx), anchor: 'start', weight: 700 });
  if (paged) out.push({ t: 'text', x: seqRight, y: 21, s: paged, size: 12, fill: hex(k.tx3), anchor: 'end' });
  if (!o.noStatus) {
    out.push({ t: 'path', d: 'M152 21v-3M157 21v-6M162 21v-9M167 21v-12', stroke: hex(k.acc), sw: 2.2 });
    out.push({ t: 'text', x: 198, y: 21, s: '72%', size: 12, fill: hex(k.tx2), anchor: 'end' });
    out.push({ t: 'rect', x: 202, y: 10, w: 24, h: 12, r: 2.5, fill: 'none', stroke: hex(k.tx2), sw: 1.2 });
    out.push({ t: 'rect', x: 204.5, y: 12.5, w: 14, h: 7, r: 1, fill: hex(k.acc) });
    out.push({ t: 'rect', x: 226.5, y: 13.5, w: 2, h: 5, r: 1, fill: hex(k.tx2) });
  }
  out.push({ t: 'rect', x: 0, y: BAR_H - 1, w: SCREEN_W, h: 1, r: 0, fill: hex(k.s2) });
}

/** Brand logos (icons 65..91, harmony_core/icons.py BRAND_COLORS) are drawn in the brand colour unless it is too dark for the fill (ui.c brand_rgb). */
const BRAND_RGB = [0xE5A00D, 0xFF0000, 0xE50914, 0x00A8E1, 0x113CCF, 0x002BE7, 0x1CE783, 0x000000, 0x0064FF, 0x000000, 0xF47521, 0x9146FF, 0x00A4DC, 0x52B54B, 0x17B2E7, 0x282A2D, 0x1DB954, 0xFA243C, 0x000000, 0xFF5500, 0x171A21, 0x107C10, 0x003791, 0xE60012, 0x76B900, 0x000000, 0x4285F4];
export function brandColour(icon: number, fill: number): number | null {
  const c = BRAND_RGB[icon - 65];
  if (c === undefined) return null;
  const luma = (v: number): number => (((v >> 16) & 255) * 299 + ((v >> 8) & 255) * 587 + (v & 255) * 114) / 1000;
  const l = Math.floor(luma(c));
  const lf = Math.floor(luma(fill));
  return (Math.abs(l - lf) < 48) || l < 40 ? null : c;
}

function widgetPrims(p: V2Page, w: V2Widget, wi: number, o: RenderOpts, T: RenderTheme, out: Prim[]): void {
  const r = widgetRect(p, w);
  const x = r.x1;
  const y = r.y1;
  const bw = r.x2 - r.x1 + 1;
  const bh = r.y2 - r.y1 + 1;
  if (w.kind === 'label') {
    if (p.version === 1 && wi === titleIndex(p)) return;
    const col = labelColor(w.color, T) ?? T.tokens.tx2;
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 4.5, s: fit(w.label, 'b13', bw - 4), size: 13, fill: hex(col), anchor: 'middle', weight: 600, wi });
    return;
  }
  const active = w.bind !== null && w.role !== 'nav' && o.activeAct != null && w.bind === o.activeAct;
  const tile = w.kind === 'tile' ? sampleTile(p, w, o) : null;
  const c = widgetColors(w, { active: !!active && w.kind === 'button', ...(tile ? { state: tile.state, stale: !!tile.stale } : {}) }, T);
  const ink = hex(c.ink);
  const brand = !tile && c.border === undefined && !(active && w.kind === 'button') && !w.color ? brandColour(w.icon, c.fill) : null;
  const icon = hex(brand ?? c.icon);
  const cap = hex(c.cap);
  const k = T.tokens;
  // a 3 px active border, else a 1 px outline when the fill is too close to the ground; both drawn inside the box
  const sw = c.border !== undefined ? 3 : c.outline ? 1 : 0;
  const stroke = c.border !== undefined ? hex(c.border) : c.outline ? hex(k.tx3) : undefined;
  if (w.shape === 'monogram') {
    const d = Math.min(bw, bh) - 4;
    const cx = x + bw / 2;
    const cy = y + (bh >= 64 && !w.icon_only && w.label ? d / 2 + 2 : bh / 2);
    const rad = d / 2 - (bh >= 64 && w.label ? 8 : 0);
    out.push({ t: 'circle', x: cx, y: cy, r: rad - sw / 2, fill: hex(c.fill), stroke, sw: sw || undefined, wi });
    if (w.icon) out.push({ t: 'icon', id: w.icon, x: cx - rad * 0.5, y: cy - rad * 0.5, size: rad, color: icon, wi });
    else out.push({ t: 'text', x: cx, y: cy + rad * 0.34, s: (Array.from(w.label)[0] ?? '?').toUpperCase(), size: Math.max(12, rad * 0.95), fill: ink, anchor: 'middle', weight: 600, wi });
    if (bh >= 64 && w.label && !w.icon_only) out.push({ t: 'text', x: cx, y: y + bh - 4, s: fit(w.label, 'r12', bw), size: 12, fill: cap, anchor: 'middle', wi });
    return;
  }
  const compact = !!(w as { compact?: boolean }).compact;
  const cls = wclass(w, compact, bw, bh);
  const radius = w.shape === 'pill' ? Math.min(bh / 2, 26) : cls === 'card' || cls === 'ccard' || tileKind(w) ? 14 : 10;
  out.push({ t: 'rect', x: x + sw / 2, y: y + sw / 2, w: bw - sw, h: bh - sw, r: Math.max(0, radius - sw / 2), fill: hex(c.fill), stroke, sw: sw || undefined, wi });
  const hasIcon = w.icon > 0;
  const base = (top: number, font: Font): number => top + lineHeight(font) * 0.78; // text baseline from the top of its line box
  if (tile) {
    // remote/app/ui.c build_tile: glyph 20 top left, state r12 top right, name m16 at the bottom
    const roomy = bh >= 56 && bw >= 80;
    const showIcon = hasIcon && roomy;
    if (showIcon) out.push({ t: 'icon', id: w.icon, x: x + 8, y: y + 8, size: 20, color: icon, wi });
    const sx = showIcon ? 34 : 10;
    const st = fit(tile.text, 'r12', bw - sx - 10);
    out.push({ t: 'text', x: x + bw - 10, y: base(y + (roomy ? 12 : (bh - lineHeight('r12')) / 2), 'r12'), s: st, size: 12, fill: cap, anchor: 'end', wi });
    if (w.label) {
      const nm = fit(w.label, 'm16', roomy ? bw - 20 : bw - 64);
      out.push({ t: 'text', x: x + 10, y: base(y + (roomy ? bh - 10 - lineHeight('m16') : (bh - lineHeight('m16')) / 2), 'm16'), s: nm, size: 16, fill: ink, anchor: 'start', weight: 500, wi });
    }
    return;
  }
  const legacyDanger = p.version === 1 && w.kind === 'button' && /^power off$/i.test(w.label);
  const text = legacyDanger ? hex(k.bad) : ink;
  const label = w.icon_only && hasIcon ? '' : w.label;
  const fixed = isFixed(p);
  const al = fixed ? 'left' : w.align ?? 'center';
  const px = cls === 'card' ? 32 : (cls === 'key' && bh < 48) || (cls === 'row' && bh < 36) ? 20 : 24;
  if (hasIcon && (!label || cls === 'pill')) {
    out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - px / 2, y: y + bh / 2 - px / 2, size: px, color: icon, wi });
    return;
  }
  if (!label) return;
  if (cls === 'row') {
    const chev = !!w.trailing;
    let x0 = 10;
    const gap = hasIcon ? px + 10 : 0;
    let lw = Math.max(0, bw - x0 - gap - 8 - (chev ? 30 : 0));
    if (chev) out.push({ t: 'icon', id: 8, x: x + bw - 8 - 20, y: y + bh / 2 - 10, size: 20, color: cap, wi });
    const s = fit(label, 'm16', lw);
    if (al !== 'left') {
      const room = bw - (chev ? 8 + 20 + 10 : 0);
      const tw = Math.min(lw, textPx(s, 'm16') + 2);
      lw = tw;
      if (al === 'right') x0 = room - gap - lw - 10 >= 10 ? room - gap - lw - 10 : x0;
      else if (room - gap - lw >= 20) x0 = Math.floor((room - gap - lw) / 2);
    }
    if (hasIcon) out.push({ t: 'icon', id: w.icon, x: x + x0, y: y + bh / 2 - px / 2, size: px, color: icon, wi });
    out.push({ t: 'text', x: x + x0 + gap, y: y + bh / 2 + 5.6, s, size: 16, fill: text, anchor: 'start', weight: 500, wi });
  } else if (cls === 'card') {
    const lh = lineHeight('b13');
    const ix = al === 'right' ? x + bw - 12 - px : al === 'left' ? x + 12 : x + (bw - px) / 2;
    const anchor = al === 'left' ? 'start' : al === 'right' ? 'end' : 'middle';
    const tx = al === 'left' ? x + 12 : al === 'right' ? x + bw - 12 : x + bw / 2;
    if (hasIcon) {
      out.push({ t: 'icon', id: w.icon, x: ix, y: y + 12, size: px, color: icon, wi });
      out.push({ t: 'text', x: tx, y: base(y + bh - 12 - lh, 'b13'), s: fit(label, 'b13', bw - 24), size: 13, fill: text, anchor, weight: 700, wi });
    } else {
      out.push({ t: 'text', x: tx, y: base(y + (bh - lineHeight('m16')) / 2, 'm16'), s: fit(label, 'm16', bw - 24), size: 16, fill: text, anchor, weight: 500, wi });
    }
  } else if (cls === 'ccard') {
    const x0 = hasIcon ? 10 + px + 10 : 10;
    const lines = wrap2(label, 'b13', bw - x0 - 8);
    const lh = lineHeight('b13');
    if (hasIcon) out.push({ t: 'icon', id: w.icon, x: x + 10, y: y + bh / 2 - px / 2, size: px, color: icon, wi });
    const top = y + (bh - lines.length * lh) / 2;
    lines.forEach((ln, i) => out.push({ t: 'text', x: x + x0, y: base(top + i * lh, 'b13'), s: ln, size: 13, fill: text, anchor: 'start', weight: 700, wi }));
  } else if (cls === 'pill') {
    const f: Font = Array.from(label).length <= 2 ? 'b20' : 'm16';
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + FONT_SIZE[f] * 0.35, s: fit(label, f, bw - 8), size: FONT_SIZE[f], fill: text, anchor: 'middle', weight: FONT_WEIGHT[f], wi });
  } else if (hasIcon) {
    // key: glyph over an r12 caption
    const lh = lineHeight('r12');
    const gap = bh >= 52 ? 4 : 2;
    const top = Math.max(0, (bh - px - gap - lh) / 2);
    out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - px / 2, y: y + top, size: px, color: icon, wi });
    out.push({ t: 'text', x: x + bw / 2, y: base(y + top + px + gap, 'r12'), s: fit(label, 'r12', bw - 6), size: 12, fill: cap, anchor: 'middle', wi });
  } else {
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 4.5, s: fit(label, 'b13', bw - 6), size: 13, fill: text, anchor: 'middle', weight: 700, wi });
  }
}

export type WClass = 'mono' | 'row' | 'pill' | 'card' | 'ccard' | 'key';
const tileKind = (w: V2Widget): boolean => w.kind === 'tile';
/** The widget class the remote draws (wclass in remote/app/ui.c): fixes the glyph size, the fonts and the layout. */
export function wclass(w: Pick<V2Widget, 'shape'>, compact: boolean, bw: number, bh: number): WClass {
  if (w.shape === 'monogram') return 'mono';
  if (w.shape === 'row' || (bh < 44 && bw >= 100)) return 'row';
  if (w.shape === 'pill') return 'pill';
  if (bw >= 100) return compact || bh < 72 ? 'ccard' : 'card';
  return 'key';
}
/** A label wrapped to at most two lines in `px` (the remote's compact card): the second line ends in "..." when cut. */
function wrap2(s: string, font: Font, px: number): string[] {
  const words = s.split(/\s+/).filter(Boolean);
  const lines: string[] = [];
  let cur = '';
  let i = 0;
  for (; i < words.length; i++) {
    const t = cur ? `${cur} ${words[i]}` : (words[i] as string);
    if (cur && textPx(t, font) > px) break;
    cur = t;
  }
  if (!cur) return [fit(s, font, px)];
  lines.push(cur);
  if (i < words.length) lines.push(fit(words.slice(i).join(' '), font, px));
  return lines.map((l) => fit(l, font, px)).slice(0, 2);
}

/** Does `align` change how this button is drawn? Only for rows and big cards of a grid page; a FIXED page always reads left to right (remote/app/ui.c). */
export function alignApplies(page: { cols: number; rows: number; fixed?: boolean }, w: { type?: string; shape: string; icon_only?: boolean; x: number; y: number; w: number; h: number }): boolean {
  if (page.fixed || w.type === 'tile' || w.type === 'label' || w.icon_only || w.shape === 'monogram') return false;
  const r = widgetRect({ version: 2, cols: page.cols, rows: page.rows, widgets: [] } as unknown as V2Page, w);
  const bw = r.x2 - r.x1 + 1;
  const bh = r.y2 - r.y1 + 1;
  return w.shape === 'row' || (bh < 44 && bw >= 100) || (w.shape === 'card' && bw >= 100 && bh >= 72);
}

/** Studio: a dashed red outline and a "-N px" badge on a widget whose label the remote would cut. */
function flagPrims(p: V2Page, w: V2Widget, wi: number, out: Prim[]): void {
  const sh = rowShift(p);
  const f = fitLabel({ cols: p.cols, rows: p.rows - sh, fixed: isFixed(p) }, { ...w, y: w.y - sh });
  if (!f) return;
  const r = widgetRect(p, w);
  const text = `\u2212${f.over} px`;
  const bwid = textPx(`-${f.over} px`, 'r12') + 8;
  out.push({ t: 'rect', x: r.x1 - 1, y: r.y1 - 1, w: r.x2 - r.x1 + 3, h: r.y2 - r.y1 + 3, r: 9, fill: 'none', stroke: DANGER, sw: 1.5, dash: true, wi });
  out.push({ t: 'rect', x: r.x2 - bwid + 2, y: r.y1 - 5, w: bwid, h: 12, r: 3, fill: DANGER, wi });
  out.push({ t: 'text', x: r.x2 - bwid / 2 + 2, y: r.y1 + 4, s: text, size: 9, fill: '#ffffff', anchor: 'middle', weight: 700, wi });
}

export function layoutPage(p: V2Page, opts: RenderOpts = {}): Scene {
  const T = opts.theme ?? themeOfPage(p);
  const prims: Prim[] = [{ t: 'rect', x: 0, y: 0, w: SCREEN_W, h: SCREEN_H, r: 0, fill: hex(T.tokens.bg) }];
  barPrims(p, opts, T, prims);
  p.widgets.forEach((w, i) => {
    widgetPrims(p, w, i, opts, T, prims);
    // not enabled by the Studio any more (amber issue dot); kept for renderer-fit tests and re-enable
    if (opts.flagOverflow) flagPrims(p, w, i, prims);
  });
  return { w: SCREEN_W, h: SCREEN_H, prims };
}

export function primTemplate(q: Prim): TemplateResult {
  switch (q.t) {
    case 'rect':
      return svg`<rect x=${q.x} y=${q.y} width=${q.w} height=${q.h} rx=${q.r} fill=${q.fill} stroke=${q.stroke ?? 'none'} stroke-width=${q.sw ?? 0} stroke-dasharray=${q.dash ? '4 3' : 'none'}></rect>`;
    case 'circle':
      return svg`<circle cx=${q.x} cy=${q.y} r=${q.r} fill=${q.fill} stroke=${q.stroke ?? 'none'} stroke-width=${q.sw ?? 0}></circle>`;
    case 'text':
      return svg`<text x=${q.x} y=${q.y} font-size=${q.size} fill=${q.fill} text-anchor=${q.anchor} font-weight=${q.weight ?? 400}>${q.s}</text>`;
    case 'icon':
      return svg`<g transform=${`translate(${q.x} ${q.y}) scale(${q.size / 24})`} fill="none" color=${q.color} stroke=${q.color} stroke-width=${1.8} stroke-linecap="round" stroke-linejoin="round">${iconInner(q.id)}</g>`;
    case 'path':
      return svg`<path d=${q.d} fill=${q.fill ?? 'none'} stroke=${q.stroke} stroke-width=${q.sw} stroke-linecap="round" stroke-linejoin="round"></path>`;
  }
}

/** The page as an SVG <g> in 240x320 screen coordinates (embed it in any <svg>). */
export function pageGroup(p: V2Page, opts: RenderOpts = {}): TemplateResult {
  return svg`<g class="rp" font-family="system-ui, -apple-system, 'Segoe UI', Roboto, Ubuntu, sans-serif">${layoutPage(p, opts).prims.map(primTemplate)}</g>`;
}

/** A standalone <svg> of the page at an integer scale (1x, 2x, 3x) or an explicit pixel width. */
export function pageSvg(p: V2Page, opts: RenderOpts = {}, scale = 2, label = '', marks: Rect[] = []): TemplateResult {
  const outlines = marks.map((r) => svg`<rect x=${r.x1 - 1} y=${r.y1 - 1} width=${r.x2 - r.x1 + 3} height=${r.y2 - r.y1 + 3} rx="6" fill="none" stroke="#f8c040" stroke-width="3" pointer-events="none"></rect>`);
  return svg`<svg class="page-svg" viewBox="0 0 240 320" width=${SCREEN_W * scale} height=${SCREEN_H * scale} role="img" aria-label=${label || `Page ${p.title || 'Harmony'}`}>${pageGroup(p, opts)}${outlines}</svg>`;
}
