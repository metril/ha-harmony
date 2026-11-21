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

/** The widget's cell box in screen pixels, before the inset. */
export function widgetBox(p: V2Page, w: Pick<V2Widget, 'x' | 'y' | 'w' | 'h'>): Rect {
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
export function widgetRect(p: V2Page, w: Pick<V2Widget, 'x' | 'y' | 'w' | 'h'>): Rect {
  const b = widgetBox(p, w);
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
  const c = cellAt(p, px, py);
  if (!c) return -1;
  return p.widgets.findIndex((w) => c.x >= w.x && c.x < w.x + w.w && c.y >= w.y && c.y < w.y + w.h);
}

const fit = (s: string, px: number, size: number): string => {
  const chars = Array.from(s);
  const max = Math.max(1, Math.floor(px / (size * 0.54)));
  return chars.length <= max ? s : chars.slice(0, Math.max(0, max - 1)).join('') + '…';
};

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
  out.push({ t: 'text', x: tx, y: 22, s: fit(title, room, 16), size: 16, fill: hex(k.tx), anchor: 'start', weight: 500 });
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
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 4.5, s: fit(w.label, bw - 4, 13), size: 13, fill: hex(col), anchor: 'middle', weight: 600, wi });
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
    if (bh >= 64 && w.label && !w.icon_only) out.push({ t: 'text', x: cx, y: y + bh - 4, s: fit(w.label, bw, 12), size: 12, fill: cap, anchor: 'middle', wi });
    return;
  }
  out.push({ t: 'rect', x: x + sw / 2, y: y + sw / 2, w: bw - sw, h: bh - sw, r: Math.max(0, radius - sw / 2), fill: hex(c.fill), stroke, sw: sw || undefined, wi });
  const size = bh < 30 || bw < 56 ? 12 : bw >= 100 ? 16 : 14;
  const hasIcon = w.icon > 0;
  if (tile) {
    const val = tile.text;
    if (w.icon_only && hasIcon) out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - 12, y: y + bh / 2 - 12, size: 24, color: icon, wi });
    else if (bh >= 52) {
      if (hasIcon) out.push({ t: 'icon', id: w.icon, x: x + 8, y: y + 8, size: 16, color: icon, wi });
      if (!w.icon_only) out.push({ t: 'text', x: x + (hasIcon ? 28 : 10), y: y + 20, s: fit(w.label, bw - (hasIcon ? 36 : 18), 12), size: 12, fill: cap, anchor: 'start', wi });
      out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 18, s: fit(val, bw - 12, 22), size: 22, fill: ink, anchor: 'middle', weight: 600, wi });
    } else out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 5, s: fit(`${w.label}  ${val}`, bw - 8, size), size, fill: ink, anchor: 'middle', wi });
    return;
  }
  const legacyDanger = p.version === 1 && w.kind === 'button' && /^power off$/i.test(w.label);
  const text = legacyDanger ? hex(k.bad) : ink;
  const label = w.icon_only ? '' : w.label;
  if (hasIcon && !label) out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - 12, y: y + bh / 2 - 12, size: 24, color: icon, wi });
  else if (hasIcon && w.shape === 'card' && bh >= 56) {
    out.push({ t: 'icon', id: w.icon, x: x + bw / 2 - 11, y: y + bh / 2 - 22, size: 22, color: icon, wi });
    out.push({ t: 'text', x: x + bw / 2, y: y + bh / 2 + 22, s: fit(label, bw - 8, size), size, fill: text, anchor: 'middle', weight: 500, wi });
  } else if (hasIcon) {
    const left = (w.shape === 'row' || w.shape === 'pill') && !w.align_center;
    const isz = 20;
    const room = bw - 16 - isz - 8;
    const s = fit(label, room, size);
    const tw = Math.min(room, s.length * size * 0.54);
    const gx = left ? x + 12 : x + bw / 2 - (isz + 8 + tw) / 2;
    out.push({ t: 'icon', id: w.icon, x: gx, y: y + bh / 2 - isz / 2, size: isz, color: icon, wi });
    out.push({ t: 'text', x: gx + isz + 8, y: y + bh / 2 + size * 0.35, s, size, fill: text, anchor: 'start', weight: 500, wi });
  } else if (label) {
    const left = w.shape === 'row' && !w.align_center;
    out.push({ t: 'text', x: left ? x + 14 : x + bw / 2, y: y + bh / 2 + size * 0.35, s: fit(label, bw - 12, size), size, fill: text, anchor: left ? 'start' : 'middle', weight: 500, wi });
  }
}

export function layoutPage(p: V2Page, opts: RenderOpts = {}): Scene {
  const T = opts.theme ?? themeOfPage(p);
  const prims: Prim[] = [{ t: 'rect', x: 0, y: 0, w: SCREEN_W, h: SCREEN_H, r: 0, fill: hex(T.tokens.bg) }];
  barPrims(p, opts, T, prims);
  p.widgets.forEach((w, i) => widgetPrims(p, w, i, opts, T, prims));
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
      return svg`<g transform=${`translate(${q.x} ${q.y}) scale(${q.size / 24})`} fill="none" stroke=${q.color} stroke-width=${1.8} stroke-linecap="round" stroke-linejoin="round">${iconInner(q.id)}</g>`;
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
