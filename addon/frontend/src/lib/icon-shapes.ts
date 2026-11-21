// Stock icon markup (icons.json) parsed once into plain shapes, so icons render as Lit SVG templates
// without raw HTML. The set only uses <path>, <rect> and <circle>; a path with a fill is a filled brand logo (drawn in currentColor).
import { svg, type TemplateResult } from 'lit';
import { stockIconById } from './stock-icons';

// Brand logos (ids with a `brand` colour) keep their markup in their own chunk, so the first download stays small.
let brandSvg: Record<string, string> = {};
export const brandsLoaded: Promise<void> = import('@shared/brand-icons.json').then((m) => {
  brandSvg = m.default;
  cache.clear();
});

export type Shape = { tag: 'path'; d: string; filled?: boolean; eo?: boolean } | { tag: 'rect'; x: number; y: number; w: number; h: number; rx: number } | { tag: 'circle'; cx: number; cy: number; r: number };

const attrs = (s: string): Record<string, string> => Object.fromEntries([...s.matchAll(/([\w-]+)="([^"]*)"/g)].map((m) => [m[1] as string, m[2] as string]));
export function parseShapes(markup: string): Shape[] {
  const out: Shape[] = [];
  for (const m of markup.matchAll(/<(path|rect|circle)\b([^>]*)\/?>/g)) {
    const a = attrs(m[2] as string);
    const n = (k: string) => Number(a[k] ?? 0);
    if (m[1] === 'path') out.push(a['fill'] && a['fill'] !== 'none' ? { tag: 'path', d: a['d'] ?? '', filled: true, ...(a['fill-rule'] === 'evenodd' ? { eo: true } : {}) } : { tag: 'path', d: a['d'] ?? '' });
    else if (m[1] === 'rect') out.push({ tag: 'rect', x: n('x'), y: n('y'), w: n('width'), h: n('height'), rx: n('rx') });
    else out.push({ tag: 'circle', cx: n('cx'), cy: n('cy'), r: n('r') });
  }
  return out;
}
const cache = new Map<number, Shape[]>();
export function shapesFor(id: number): Shape[] {
  let s = cache.get(id);
  if (!s) {
    s = parseShapes(brandSvg[String(id)] ?? stockIconById(id)?.svg ?? '');
    cache.set(id, s);
  }
  return s;
}
export const shapeTemplate = (s: Shape): TemplateResult =>
  s.tag === 'path' ? (s.filled ? svg`<path d=${s.d} fill="currentColor" fill-rule=${s.eo ? 'evenodd' : 'nonzero'} stroke="none"></path>` : svg`<path d=${s.d}></path>`) : s.tag === 'rect' ? svg`<rect x=${s.x} y=${s.y} width=${s.w} height=${s.h} rx=${s.rx}></rect>` : svg`<circle cx=${s.cx} cy=${s.cy} r=${s.r}></circle>`;
/** The icon's strokes, to be placed inside any <svg> or <g> that sets stroke/fill/transform. */
export const iconInner = (id: number): TemplateResult[] => shapesFor(id).map(shapeTemplate);
/** A standalone 24-unit icon element (palette, inspector). */
export const stockIconSvg = (id: number, cls = 'sic'): TemplateResult =>
  svg`<svg class=${cls} viewBox="0 0 24 24" aria-hidden="true" focusable="false">${iconInner(id)}</svg>`;
