// The assignment state machine behind all three ways to assign (drag, click-then-click, keyboard):
// idle -> picked(source) -> idle on commit/cancel, and idle/picked -> dragging(source) -> idle on drop/cancel.
// Every path ends in the same `commit(source, target)`.
import type { Source, Target } from './studio-model';

export type Phase = { name: 'idle' } | { name: 'picked'; source: Source } | { name: 'dragging'; source: Source; x: number; y: number; over: Target | null; from: 'idle' | 'picked' };
export type Commit = (source: Source, target: Target) => void;

export class Assigner extends EventTarget {
  phase: Phase = { name: 'idle' };
  /** target highlighted by the keyboard cursor (or the pointer while dragging) */
  cursor: Target | null = null;
  constructor(private onCommit: Commit) {
    super();
  }
  private emit() {
    this.dispatchEvent(new Event('change'));
  }
  get source(): Source | null {
    return this.phase.name === 'idle' ? null : this.phase.source;
  }
  get active(): boolean {
    return this.phase.name !== 'idle';
  }

  /** Click / tap / Space on a source: pick it up; the same source again puts it down. */
  pick(source: Source): void {
    const cur = this.phase;
    if (cur.name === 'picked' && JSON.stringify(cur.source) === JSON.stringify(source)) return this.cancel();
    this.phase = { name: 'picked', source };
    this.cursor = null;
    this.emit();
  }
  /** Click / tap / Enter on a target while something is picked up. Returns whether it was committed. */
  choose(target: Target): boolean {
    if (this.phase.name !== 'picked') return false;
    const { source } = this.phase;
    this.reset();
    this.onCommit(source, target);
    return true;
  }
  startDrag(source: Source, x: number, y: number): void {
    const from = this.phase.name === 'picked' ? 'picked' : 'idle';
    this.phase = { name: 'dragging', source, x, y, over: null, from };
    this.emit();
  }
  moveDrag(x: number, y: number, over: Target | null): void {
    if (this.phase.name !== 'dragging') return;
    this.phase = { ...this.phase, x, y, over };
    this.cursor = over;
    this.emit();
  }
  /** Pointer released: commit on a valid target, otherwise the drag just ends. */
  drop(target: Target | null): boolean {
    if (this.phase.name !== 'dragging') return false;
    const { source } = this.phase;
    this.reset();
    if (!target) return false;
    this.onCommit(source, target);
    return true;
  }
  /** Esc, or clicking away. */
  cancel(): void {
    if (this.phase.name === 'idle') return;
    this.reset();
  }
  private reset() {
    this.phase = { name: 'idle' };
    this.cursor = null;
    this.emit();
  }
  setCursor(t: Target | null) {
    this.cursor = t;
    this.emit();
  }
}

export interface Spot {
  id: string;
  target: Target;
  x: number;
  y: number;
}
/** Arrow-key movement between targets: the nearest spot in that direction (a cone around the axis). */
export function moveCursor(spots: Spot[], current: string | null, dir: 'left' | 'right' | 'up' | 'down'): Spot | null {
  if (!spots.length) return null;
  const cur = spots.find((s) => s.id === current);
  if (!cur) return spots[0] as Spot;
  let best: Spot | null = null;
  let bestScore = Infinity;
  for (const s of spots) {
    if (s === cur) continue;
    const dx = s.x - cur.x;
    const dy = s.y - cur.y;
    const along = dir === 'left' ? -dx : dir === 'right' ? dx : dir === 'up' ? -dy : dy;
    const across = dir === 'left' || dir === 'right' ? Math.abs(dy) : Math.abs(dx);
    if (along <= 0.5 || across > along * 2.5 + 8) continue;
    const score = along + across * 2;
    if (score < bestScore) {
      bestScore = score;
      best = s;
    }
  }
  return best ?? cur;
}
