// X-ray mode: every key labelled with its action, labels in two columns beside the remote with leader lines.
import { keyByName } from './remote-layout';

export interface XrayLabel {
  key: string;
  side: 'left' | 'right';
  /** label anchor (text starts at x on the right, ends at x on the left) */
  x: number;
  y: number;
  /** leader line from the key edge to the label */
  x1: number;
  y1: number;
  text: string;
}
export const XRAY_PAD = 215;
const MIN_GAP = 18;

/** Place one label per key that has text. Keys left of the centre label on the left, the others on the right;
 *  labels are pushed apart vertically so none overlap. */
export function xrayLabels(items: { key: string; text: string }[], centre = 150): XrayLabel[] {
  const out: XrayLabel[] = [];
  const sides = new Map<string, 'left' | 'right'>();
  let mid = 0;
  [...items]
    .sort((a, b) => (keyByName.get(a.key)?.y ?? 0) - (keyByName.get(b.key)?.y ?? 0))
    .forEach((it) => {
      const g = keyByName.get(it.key);
      if (!g) return;
      const cx = g.x + g.w / 2;
      sides.set(it.key, cx < centre - 25 ? 'left' : cx > centre + 25 ? 'right' : mid++ % 2 ? 'right' : 'left');
    });
  for (const side of ['left', 'right'] as const) {
    const col = items
      .map((it) => ({ it, g: keyByName.get(it.key) }))
      .filter((v): v is { it: { key: string; text: string }; g: NonNullable<typeof v.g> } => !!v.g && !!v.it.text && sides.get(v.it.key) === side)
      .map((v) => ({ ...v, cy: v.g.y + v.g.h / 2 }))
      .sort((a, b) => a.cy - b.cy || a.g.x - b.g.x);
    let prev = -Infinity;
    for (const v of col) {
      const y = Math.max(v.cy, prev + MIN_GAP);
      prev = y;
      const edge = side === 'left' ? v.g.x : v.g.x + v.g.w;
      out.push({ key: v.it.key, side, x: side === 'left' ? -12 : 312, y, x1: edge, y1: v.cy, text: v.it.text });
    }
  }
  return out;
}
