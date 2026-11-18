// Pointer-based reordering along one axis (touch-friendly; HTML5 drag and drop is not used).
// `items()` returns the elements in order; the drop index is the nearest element centre to the pointer.
export function dropIndex(centres: number[], pos: number): number {
  let best = 0;
  let d = Infinity;
  centres.forEach((c, i) => {
    const k = Math.abs(c - pos);
    if (k < d) {
      d = k;
      best = i;
    }
  });
  return best;
}

export interface ReorderOpts {
  items: () => HTMLElement[];
  from: number;
  onOver: (to: number) => void;
  onDrop: (from: number, to: number) => void;
  /** movement in px before it counts as a drag (below it, the release is a click) */
  slop?: number;
}
/** Start tracking a drag that began with `e`; returns false when the gesture was a plain click. */
export function startReorder(e: PointerEvent, o: ReorderOpts): void {
  const x0 = e.clientX;
  const y0 = e.clientY;
  let moved = false;
  let to = o.from;
  const at = (ev: PointerEvent) => dropIndex(o.items().map((el) => { const r = el.getBoundingClientRect(); return r.left + r.width / 2 + (r.top + r.height / 2) * 0.001; }), ev.clientX);
  const move = (ev: PointerEvent) => {
    if (!moved && Math.hypot(ev.clientX - x0, ev.clientY - y0) < (o.slop ?? 6)) return;
    moved = true;
    to = at(ev);
    o.onOver(to);
  };
  const end = (ev: PointerEvent) => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', end);
    window.removeEventListener('pointercancel', cancel);
    if (moved) o.onDrop(o.from, at(ev));
  };
  const cancel = () => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', end);
    window.removeEventListener('pointercancel', cancel);
    o.onOver(-1);
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', end);
  window.addEventListener('pointercancel', cancel);
}
