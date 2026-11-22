// Label pixel budgets on the Elite's 240x320 screen: what fits and what the remote clips.
// A port of ra_ui_widget_rect and the class font / label-width rules of build_v2_content / build_tile (remote/app/ui.c), measured with the
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

/** The baked font's line height in px. */
export const lineHeight = (font: Font): number => M[font]['lh'] as number;

/** Width of `s` in `font`; a code point the font lacks is LVGL's placeholder, line_height / 2 + 2. */
export function textPx(s: string, font: Font): number {
  const m = M[font];
  const fb = Math.floor((m['lh'] as number) / 2) + 2; // LV_USE_FONT_PLACEHOLDER (lv_font.c)
  let w = 0;
  for (const c of s) w += m[String(c.codePointAt(0))] ?? fb;
  return w;
}

/** What LVGL's LONG_DOT mode shows in `px`: the text (ending in "..." when cut) and the pixels over. */
export function clip(s: string, font: Font, px: number): { text: string; over: number } {
  const w = textPx(s, font);
  if (w <= px) return { text: s, over: 0 };
  const chars = Array.from(s);
  if (chars.length <= DOTS.length) return { text: s, over: w - px }; // lv_label: never turn all the letters into dots
  const dots = textPx(DOTS, font);
  while (chars.length && textPx(chars.join(''), font) + dots > px) chars.pop();
  // lv_label.c: "be sure there is space for the dots" - they overwrite 3 bytes, so step back until they fit the text
  const bytes = (t: string): number => new TextEncoder().encode(t).length;
  while (chars.length && bytes(chars.join('')) + DOTS.length > bytes(s)) chars.pop();
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

export interface FitPage { cols: number; rows: number; fixed?: boolean }
export interface FitWidget { kind: string; x: number; y: number; w: number; h: number; label: string; shape: string; icon: number; icon_only: boolean; trailing?: boolean; compact?: boolean }

/** FIXED layout class size of a widget (docs/proto/page-format.md); null for a cell the layout cannot place. */
function fixedSize(w: Pick<FitWidget, 'kind' | 'x' | 'w' | 'h' | 'shape' | 'compact'>): [number, number] | null {
  const sp = w.w;
  if (w.h !== 1 || ![12, 6, 4, 3].includes(sp) || w.x % sp) return null;
  if (w.compact && (w.kind !== 'button' || w.shape !== 'card' || sp !== 6)) return null;
  const n = 12 / sp;
  const bw = Math.floor((224 - (n - 1) * 8) / n);
  const bh = w.kind === 'label' || w.shape === 'row' ? 44 : w.kind === 'tile' ? 64 : w.shape === 'pill' ? 52 : sp >= 6 ? (w.compact ? 56 : 80) : 52;
  return [bw, bh];
}

/** Width and height of a widget's drawn rectangle (v2 pages: no title-row shift); a FIXED page gives the class size. */
export function widgetSize(p: FitPage, w: Pick<FitWidget, 'kind' | 'x' | 'y' | 'w' | 'h' | 'shape' | 'compact'>): [number, number] {
  if (p.fixed) {
    const f = fixedSize(w);
    if (f) return f;
  }
  let x1 = edge(w.x, p.cols, SCREEN_W);
  let x2 = edge(w.x + w.w, p.cols, SCREEN_W) - 1;
  let y1 = BAR_H + edge(w.y, p.rows, SCREEN_H - BAR_H);
  let y2 = BAR_H + edge(w.y + w.h, p.rows, SCREEN_H - BAR_H) - 1;
  [x1, x2] = inset(x1, x2, x1 === 0 ? MARGIN : GAP / 2, x2 === SCREEN_W - 1 ? MARGIN : GAP / 2);
  [y1, y2] = inset(y1, y2, y1 === BAR_H ? MARGIN : GAP / 2, y2 === SCREEN_H - 1 ? MARGIN : GAP / 2);
  return [x2 - x1 + 1, y2 - y1 + 1];
}

/**
 * The font and maximum label pixels the remote gives this widget's label; null when it draws no label.
 * The class rules of build_v2_content / build_tile (remote/app/ui.c): row m16, card b13 (compact: two lines beside the
 * glyph), key r12 caption under a glyph, pill b20 digit, tile name m16.
 */
export function labelBudget(p: FitPage, w: FitWidget): { font: Font; px: number } | null {
  const [bw, bh] = widgetSize(p, w);
  const hasIcon = w.icon > 0;
  if (w.kind === 'tile') return w.label ? { font: 'm16', px: Math.max(0, bh >= 56 && bw >= 80 ? bw - 20 : bw - 64) } : null;
  if (w.kind !== 'button' || !w.label || w.shape === 'monogram' || (w.icon_only && hasIcon)) return null;
  if (w.shape === 'row' || (bh < 44 && bw >= 100)) {
    const px = bh < 36 ? 20 : 24;
    return { font: 'm16', px: Math.max(0, bw - 10 - (hasIcon ? px + 10 : 0) - 8 - (w.trailing ? 30 : 0)) };
  }
  if (w.shape === 'pill') return { font: Array.from(w.label).length <= 2 ? 'b20' : 'm16', px: Math.max(0, bw - 8) }; // a digit reads at b20
  if (bw >= 100) {
    if (w.compact || bh < 72) {
      // two lines beside the glyph, wrapped at spaces only: a word wider than the line would break mid-word, so nothing fits
      const line = bw - (hasIcon ? 44 : 10) - 8;
      if (w.label.split(/\s+/).some((word) => textPx(word, 'b13') > line)) return { font: 'b13', px: Math.max(0, line - 1) };
      return { font: 'b13', px: Math.max(0, 2 * line - 4) };
    }
    return { font: hasIcon ? 'b13' : 'm16', px: Math.max(0, bw - 24) };
  }
  return { font: hasIcon ? 'r12' : 'b13', px: Math.max(0, bw - 6) };
}

/** Font, budget and pixels over for a widget whose label the remote would cut; null when it fits (or has no label). */
export function fitLabel(p: FitPage, w: FitWidget): { font: Font; px: number; over: number } | null {
  const b = labelBudget(p, w);
  if (!b) return null;
  const over = textPx(w.label, b.font) - b.px;
  return over > 0 ? { ...b, over } : null;
}
