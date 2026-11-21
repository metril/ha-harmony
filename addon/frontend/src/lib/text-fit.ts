// Label pixel budgets on the Elite's 240x320 screen: what fits and what the remote clips.
// A port of ra_ui_widget_rect and the font / label-width rules of build_v2_content (remote/app/ui.c), measured with the
// baked fonts' advance widths (font_metrics.json, from tools/gen_font_metrics.py). Twin of harmony_core/text_fit.py;
// both run tests/vectors/text_fit.json.
import metrics from '@shared/font_metrics.json';

export type Font = 'b13' | 'm16' | 'b18' | 'b20' | 'r12';
export const DOTS = '...';
const SCREEN_W = 240;
const SCREEN_H = 320;
const BAR_H = 32;
const MARGIN = 8;
const GAP = 8;

const M = metrics as Record<Font, Record<string, number>>;

/** Width of `s` in `font`; a code point the font lacks counts as "?". */
export function textPx(s: string, font: Font): number {
  const m = M[font];
  const fb = m['63'] as number;
  let w = 0;
  for (const c of s) w += m[String(c.codePointAt(0))] ?? fb;
  return w;
}

/** What LVGL's LONG_DOT mode shows in `px`: the text (ending in "..." when cut) and the pixels over. */
export function clip(s: string, font: Font, px: number): { text: string; over: number } {
  const w = textPx(s, font);
  if (w <= px) return { text: s, over: 0 };
  const dots = textPx(DOTS, font);
  const chars = Array.from(s);
  while (chars.length && textPx(chars.join(''), font) + dots > px) chars.pop();
  return { text: chars.join('') + DOTS, over: w - px };
}

const edge = (i: number, n: number, total: number): number => Math.ceil((i * total) / n);
function inset(lo: number, hi: number, a: number, b: number): [number, number] {
  while (hi - lo + 1 - a - b < 12 && (a || b)) {
    if (a) a--;
    if (b) b--;
  }
  return [lo + a, hi - b];
}

export interface FitPage { cols: number; rows: number }
export interface FitWidget { kind: string; x: number; y: number; w: number; h: number; label: string; shape: string; icon: number; icon_only: boolean; trailing?: boolean }

/** Width and height of a widget's drawn rectangle (v2 pages: no title-row shift). */
export function widgetSize(p: FitPage, w: Pick<FitWidget, 'x' | 'y' | 'w' | 'h'>): [number, number] {
  let x1 = edge(w.x, p.cols, SCREEN_W);
  let x2 = edge(w.x + w.w, p.cols, SCREEN_W) - 1;
  let y1 = BAR_H + edge(w.y, p.rows, SCREEN_H - BAR_H);
  let y2 = BAR_H + edge(w.y + w.h, p.rows, SCREEN_H - BAR_H) - 1;
  [x1, x2] = inset(x1, x2, x1 === 0 ? MARGIN : GAP / 2, x2 === SCREEN_W - 1 ? MARGIN : GAP / 2);
  [y1, y2] = inset(y1, y2, y1 === BAR_H ? MARGIN : GAP / 2, y2 === SCREEN_H - 1 ? MARGIN : GAP / 2);
  return [x2 - x1 + 1, y2 - y1 + 1];
}

/** The font and maximum label pixels the remote gives this widget's label; null when it draws no label. */
export function labelBudget(p: FitPage, w: FitWidget): { font: Font; px: number } | null {
  const [bw, bh] = widgetSize(p, w);
  const hasIcon = w.icon > 0;
  if (w.kind === 'tile') {
    if (!(bh >= 44 && w.label)) return null;
    return { font: 'r12', px: bw - (hasIcon && bh >= 56 && bw >= 56 ? 38 : 8) };
  }
  if (w.kind !== 'button' || !w.label || w.shape === 'monogram' || (w.icon_only && hasIcon)) return null;
  const row = w.shape === 'row' || w.shape === 'pill' || (bh < 44 && bw >= 100);
  const big = w.shape === 'card' && !row && bw >= 100 && bh >= 80;
  const px = row ? 24 : big ? 32 : bh >= 64 ? 24 : 20;
  let font: Font;
  let lw: number;
  if (big) {
    font = 'b18';
    lw = bw - 24;
  } else {
    font = row || bw >= 100 ? 'm16' : 'b13';
    lw = bw - 8;
  }
  if (row) {
    // a keypad pill with a 1-2 character label (a digit) is set in b20
    if (w.shape === 'pill' && Array.from(w.label).length <= 2) font = 'b20';
    lw = hasIcon ? bw - 12 - px - 16 : bw - 24;
    if (w.trailing) lw -= 28;
  }
  return { font, px: Math.max(0, lw) };
}

/** Font, budget and pixels over for a widget whose label the remote would cut; null when it fits (or has no label). */
export function fitLabel(p: FitPage, w: FitWidget): { font: Font; px: number; over: number } | null {
  const b = labelBudget(p, w);
  if (!b) return null;
  const over = textPx(w.label, b.font) - b.px;
  return over > 0 ? { ...b, over } : null;
}
