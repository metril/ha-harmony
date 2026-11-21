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
import { clip, fitLabel, labelBudget, lineHeight, textPx, type Font } from './text-fit';
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
  /** studio: outline every label the remote would cut, with a -N px badge */
  flagOverflow?: boolean;
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
  let tx = 12;
  if (p.back) {
    out.push({ t: 'path', d: 'M16 10l-6 6 6 6', stroke: hex(k.tx2), sw: 2 });
    tx = 26;
  }
  const paged = p.seq ? `${p.seq.idx}/${p.seq.cnt}` : '';
  const room = (o.noStatus ? 228 : paged ? 126 : 140) - tx;
  out.push({ t: 'text', x: tx, y: 22, s: fit(title, 'm16', room), size: 16, fill: hex(k.tx), anchor: 'start', weight: 500 });
  if (paged) out.push({ t: 'text', x: o.noStatus ? 228 : 146, y: 21, s: paged, size: 12, fill: hex(k.tx2), anchor: 'end', weight: 600 });
  if (!o.noStatus) {
    out.push({ t: 'path', d: 'M152 21v-3M157 21v-6M162 21v-9M167 21v-12', stroke: hex(k.acc), sw: 2.2 });
    out.push({ t: 'text', x: 198, y: 21, s: '72%', size: 12, fill: hex(k.tx2), anchor: 'end' });
    out.push({ t: 'rect', x: 202, y: 10, w: 24, h: 12, r: 2.5, fill: 'none', stroke: hex(k.tx2), sw: 1.2 });
    out.push({ t: 'rect', x: 204.5, y: 12.5, w: 14, h: 7, r: 1, fill: hex(k.acc) });
    out.push({ t: 'rect', x: 226.5, y: 13.5, w: 2, h: 5, r: 1, fill: hex(k.tx2) });
  }
  out.push({ t: 'rect', x: 0, y: BAR_H - 1, w: SCREEN_W, h: 1, r: 0, fill: hex(k.s2) });
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
  const icon = hex(c.icon);
  const cap = hex(c.cap);
  const k = T.tokens;
  // a 3 px active border, else a 1 px outline when the fill is too close to the ground; both drawn inside the box
  const sw = c.border !== undefined ? 3 : c.outline ? 1 : 0;
  const stroke = c.border !== undefined ? hex(c.border) : c.outline ? hex(k.tx3) : undefined;
  const radius = w.shape === 'pill' ? Math.min(bh / 2, 24) : w.shape === 'monogram' ? 0 : w.shape === 'row' ? 10 : 14;
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
  out.push({ t: 'rect', x: x + sw / 2, y: y + sw / 2, w: bw - sw, h: bh - sw, r: Math.max(0, radius - sw / 2), fill: hex(c.fill), stroke, sw: sw || undefined, wi });
  const sh = rowShift(p);
  const lb = labelBudget({ cols: p.cols, rows: p.rows - sh }, { ...w, y: w.y - sh });
  const font: Font = lb?.font ?? 'b13';
  const size = FONT_SIZE[font];
  const weight = FONT_WEIGHT[font];
  const room0 = lb?.px ?? 0;
  const hasIcon = w.icon > 0;
  // the remote's layout choice (remote/app/ui.c): a row/pill or any wide, short button is one line; a big card stacks
  const rowPath = w.shape === 'row' || w.shape === 'pill' || (bh < 44 && bw >= 100);
  const big = w.shape === 'card' && !rowPath && bw >= 100 && bh >= 80;
  if (tile) {
    const val = tile.text;
    const vf: Font = bw >= 80 ? 'b20' : 'b13';
    // remote/app/ui.c: a tile ignores icon_only; the icon needs bh >= 56 and bw >= 56, the caption bh >= 44
    const showIcon = hasIcon && bh >= 56 && bw >= 56;
    const capH = bh >= 44 && !!w.label;
    if (showIcon) out.push({ t: 'icon', id: w.icon, x: x + 8, y: y + 8, size: 16, color: icon, wi });
    if (capH) out.push({ t: 'text', x: showIcon ? x + 32 : x + bw / 2, y: y + 20, s: fit(w.label, 'r12', bw - (showIcon ? 38 : 8)), size: 12, fill: cap, anchor: showIcon ? 'start' : 'middle', wi });
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + (capH ? 18 : FONT_SIZE[vf] * 0.35), s: fit(val, vf, bw - 8), size: FONT_SIZE[vf], fill: ink, anchor: 'middle', weight: 700, wi });
    return;
  }
  const legacyDanger = p.version === 1 && w.kind === 'button' && /^power off$/i.test(w.label);
  const text = legacyDanger ? hex(k.bad) : ink;
  const label = w.icon_only ? '' : w.label;
  if (hasIcon && !label) out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - 12, y: y + bh / 2 - 12, size: 24, color: icon, wi });
  else if (hasIcon && !rowPath) {
    // stacked: icon over label; the big card (bw >= 100, bh >= 80) honours align, a dense one is always centred
    const al = big ? w.align ?? 'center' : 'center';
    const ax = al === 'left' ? x + 12 : al === 'right' ? x + bw - 12 : x + bw / 2;
    const anchor = al === 'left' ? 'start' : al === 'right' ? 'end' : 'middle';
    if (big || bh >= 56) {
      out.push({ t: 'icon', id: w.icon, x: al === 'left' ? ax : al === 'right' ? ax - 22 : ax - 11, y: y + bh / 2 - 22, size: 22, color: icon, wi });
      out.push({ t: 'text', x: ax, y: y + bh / 2 + 22, s: fit(label, font, room0), size, fill: text, anchor, weight, wi });
    } else {
      // a short dense button (ui.c): icon at the top, label at the bottom, the free height split in thirds
      const isz = bh >= 64 ? 24 : 20;
      const gap = Math.max(0, Math.floor((bh - isz - lineHeight(font)) / 3));
      out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - isz / 2, y: y + gap + 1, size: isz, color: icon, wi });
      out.push({ t: 'text', x: x + bw / 2, y: y + bh - gap - lineHeight(font) * 0.22, s: fit(label, font, room0), size, fill: text, anchor: 'middle', weight, wi });
    }
  } else if (hasIcon) {
    const al = rowPath ? w.align ?? 'center' : 'center';
    const isz = 20;
    const chev = !!w.trailing && rowPath;
    if (chev) chevPrim(out, x, y, bw, bh, icon, wi);
    const span = chev ? bw - CHEV_ROOM : bw;
    const s = fit(label, font, room0);
    const tw = Math.min(room0, textPx(s, font));
    const gw = isz + 8 + tw;
    const gx = al === 'left' ? x + 12 : al === 'right' ? x + span - 12 - gw : x + span / 2 - gw / 2;
    out.push({ t: 'icon', id: w.icon, x: gx, y: y + bh / 2 - isz / 2, size: isz, color: icon, wi });
    out.push({ t: 'text', x: gx + isz + 8, y: y + bh / 2 + size * 0.35, s, size, fill: text, anchor: 'start', weight, wi });
  } else if (label) {
    const al = rowPath || big ? w.align ?? 'center' : 'center';
    const chev = !!w.trailing && rowPath;
    if (chev) chevPrim(out, x, y, bw, bh, icon, wi);
    const span = chev ? bw - CHEV_ROOM : bw;
    const tx = al === 'left' ? x + 12 : al === 'right' ? x + span - 12 : x + span / 2;
    const anchor = al === 'left' ? 'start' : al === 'right' ? 'end' : 'middle';
    out.push({ t: 'text', x: tx, y: y + bh / 2 + size * 0.35, s: fit(label, font, room0), size, fill: text, anchor, weight, wi });
  }
}

/** Does `align` change how this button is drawn? Only for rows/pills, wide short buttons and big cards (remote/app/ui.c). */
export function alignApplies(page: { cols: number; rows: number }, w: { type?: string; shape: string; icon_only?: boolean; x: number; y: number; w: number; h: number }): boolean {
  if (w.type === 'tile' || w.type === 'label' || w.icon_only || w.shape === 'monogram') return false;
  const r = widgetRect({ version: 2, cols: page.cols, rows: page.rows, widgets: [] } as unknown as V2Page, w);
  const bw = r.x2 - r.x1 + 1;
  const bh = r.y2 - r.y1 + 1;
  const rowPath = w.shape === 'row' || w.shape === 'pill' || (bh < 44 && bw >= 100);
  return rowPath || (w.shape === 'card' && bw >= 100 && bh >= 80);
}

/** the remote's trailing nav chevron: a 24 px glyph in a 28 px box, 8 px in from the right edge */
const CHEV_ROOM = 36;
function chevPrim(out: Prim[], x: number, y: number, bw: number, bh: number, color: string, wi: number): void {
  out.push({ t: 'icon', id: 8, x: x + bw - CHEV_ROOM + 2, y: y + bh / 2 - 12, size: 24, color, wi });
}

/** Studio: a dashed red outline and a "-N px" badge on a widget whose label the remote would cut. */
function flagPrims(p: V2Page, w: V2Widget, wi: number, out: Prim[]): void {
  const sh = rowShift(p);
  const f = fitLabel({ cols: p.cols, rows: p.rows - sh }, { ...w, y: w.y - sh });
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
