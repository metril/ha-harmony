import { LitElement, css, html, nothing, svg, type TemplateResult } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { KeymapRow, V2Page } from '../lib/api';
import type { Assigner, Spot } from '../lib/assigner';
import { cellAt, pageGroup, widgetRect } from '../lib/page-renderer';
import { activeTheme } from '../lib/theme-model';
import { BODY, DPAD, GLASS, GRIP_Y, JOINED, PLATED, PLATES, KEYS, PHYSICAL_KEYS, PRINTED, SCREEN, VIEW, bodyPath, gripPath, keyPath, keyTitle, tintPath, type KeyGeom } from '../lib/remote-layout';
import type { StudioHost } from '../lib/studio-state';
import { DEFAULT_LAYER, activityIndex, boxOf, clip, errorTarget, fits, gridOf, isList, pageDims, promotedIndex, wireMap, type Box, type Source, type Target } from '../lib/studio-model';
import { XRAY_PAD, xrayLabels } from '../lib/xray';
import type { Page } from '../lib/model';
import { collectIssues, widgetIssues } from '../lib/issues';

interface Session {
  kind: 'move' | 'resize';
  page: number;
  index: number;
  startX: number;
  startY: number;
  box: Box;
  ghost: Box;
  valid: boolean;
  moved: boolean;
  id: number;
  wasSel: boolean;
}
const tid = (t: Target): string => (t.kind === 'key' ? `k:${t.name}` : t.kind === 'cell' ? `c:${t.page}:${t.x}:${t.y}` : `w:${t.page}:${t.index}`);
export { tid };

/** Where the inherited-from-default dot sits: inside the key. Rect keys use the bbox top-right; path keys use their top-right vertex pulled toward the centroid. */
const DOTS = new Map<string, [number, number]>();
/** D-pad wedges: angle (deg, y down) of the dot on the ring, toward the wedge's top-right. */
const DPAD_DOT: Record<string, number> = { up: -62, right: -28, down: 62, left: 208 };
function dotAnchor(g: KeyGeom): [number, number] {
  const hit = DOTS.get(g.name);
  if (hit) return hit;
  let out: [number, number] = [g.x + g.w - 8, g.y + 8];
  if (g.shape === 'circle') {
    const r = g.w / 2;
    DOTS.set(g.name, (out = [g.x + r + r * 0.6, g.y + r - r * 0.6]));
    return out;
  }
  const wedge = DPAD_DOT[g.name];
  if (wedge !== undefined) {
    const t = (wedge * Math.PI) / 180, rr = DPAD.r * 0.66;
    DOTS.set(g.name, (out = [DPAD.x + rr * Math.cos(t), DPAD.y + rr * Math.sin(t)]));
    return out;
  }
  if (g.shape === 'path' && g.d) {
    const pts: [number, number][] = [];
    for (const seg of g.d.split(/(?=[A-Za-z])/)) {
      const n = (seg.slice(1).match(/-?\d+(?:\.\d+)?/g) ?? []).map(Number);
      if (seg[0] === 'A') { if (n.length >= 7) pts.push([n[5] as number, n[6] as number]); } else if (seg[0] === 'Q') { if (n.length >= 4) pts.push([n[2] as number, n[3] as number]); } else if ('ML'.includes(seg[0] as string)) for (let i = 0; i + 1 < n.length; i += 2) pts.push([n[i] as number, n[i + 1] as number]);
    }
    if (pts.length) {
      const cx = pts.reduce((a, p) => a + p[0], 0) / pts.length, cy = pts.reduce((a, p) => a + p[1], 0) / pts.length;
      const v = pts.reduce((b, p) => (p[0] - p[1] > b[0] - b[1] ? p : b));
      const dist = Math.hypot(cx - v[0], cy - v[1]) || 1;
      const k = Math.min(0.5, 10 / dist);
      out = [v[0] + (cx - v[0]) * k, v[1] + (cy - v[1]) * k];
    }
  }
  DOTS.set(g.name, out);
  return out;
}

/** The Harmony Elite drawing with the live screen, hard keys as buttons, and the screen editor overlay. */
@customElement('hh-remote')
export class HhRemote extends LitElement {
  static override styles = css`
    :host { display: block; min-width: 0; }
    :host([fit]) { height: 100%; min-height: 360px; }
    /* a selected widget and its resize handle are dragged by touch: the browser must not scroll instead */
    rect.wh.sel, rect.handle { touch-action: none; }
    svg.remote { display: block; margin: 0 auto; max-width: none; touch-action: pan-x pan-y pinch-zoom; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
    /* Fit: as large as the canvas allows (it fills the studio's height), but never past 1.25x */
    :host([fit]) svg.remote { width: 100%; height: 100%; max-height: 1125px; }
    .body { fill: url(#bodyg); stroke: #000; stroke-width: 2; }
    .grip { fill: #272a2e; pointer-events: none; }
    .seam { stroke: rgba(255, 255, 255, 0.1); stroke-width: 1.2; pointer-events: none; }
    .glass { fill: none; stroke: none; }
    .hint { fill: #55616c; font: 600 8px system-ui, sans-serif; letter-spacing: 1px; text-transform: uppercase; }
    .key { cursor: pointer; outline: none; }
    .cap { fill: url(#keyg); stroke: #15181b; stroke-width: 1; transition: fill 140ms ease-out; }
    .tint { fill: none; stroke-width: 1.8; pointer-events: none; }
    .g-strip .cap { fill: #030405; stroke: #e8edf0; stroke-width: 1.3; }
    .g-strip:hover .cap { fill: #151a1e; }
    .glow { fill: none; stroke: none; pointer-events: none; transition: fill 140ms ease-out, stroke 140ms ease-out; }
    .lab { fill: #f1f4f6; font: 700 11px system-ui, sans-serif; text-anchor: middle; dominant-baseline: central; pointer-events: none; }
    .g-power .lab { font-size: 9px; letter-spacing: 0.4px; }
    .g-strip .lab { font-size: 9px; letter-spacing: 0.5px; fill: #fff; }
    .g-home .lab { font-size: 15px; }
    /* the legend between two keys sits on its own plate-coloured backing, so a selected neighbour's outline and glow never run through it */
    .pbk { fill: #4d5257; pointer-events: none; }
    .pr { fill: #eef1f3; font: 700 11px system-ui, sans-serif; text-anchor: middle; dominant-baseline: central; pointer-events: none; }
    .prl { stroke: #eef1f3; stroke-width: 1.2; pointer-events: none; }
    .gl { fill: #eef1f3; pointer-events: none; }
    .gs { fill: none; stroke: #eef1f3; stroke-width: 1.6; stroke-linecap: round; stroke-linejoin: round; pointer-events: none; }
    .gs.rec { stroke: #ff8a7e; }
    .key.sel .gs.rec, .key.over .gs.rec { stroke: #ffb3aa; }
    .mk, .dir { opacity: 0; }
    .key:hover .mk, .key:focus-visible .mk, .key.sel .mk, .key.ov .mk { opacity: 1; }
    .key:hover .dir, .key:focus-visible .dir, .key.sel .dir, .key.ov .dir { opacity: 0.9; }
    /* joined keys: one flat plate, no seam at rest; the half under the pointer or assigned shows itself */
    .jn .cap { fill: #4d5257; stroke: #4d5257; stroke-width: 0.7; }
    .plate { fill: #4d5257; stroke: #4d5257; stroke-width: 0.7; pointer-events: none; }
    .jp .cap { fill: transparent; stroke: none; }
    .key.jp:hover .cap { fill: rgba(255, 255, 255, 0.13); }
    .key.jn:hover .cap { fill: #565c62; stroke: #565c62; }
    .key.colour .lab { display: none; }
    .key:hover .cap { fill: url(#keyh); }
    .key.ov .glow { fill: none; stroke: #3ee8cf; stroke-width: 1.8; }
    .key.none .glow { fill: rgba(5, 8, 11, 0.18); }
    .key.colour.none .glow { fill: rgba(5, 8, 11, 0.2); }
    .key.ex .glow { fill: rgba(5, 8, 11, 0.4); stroke: #687888; stroke-width: 1; stroke-dasharray: 3 2; }
    .key.bad .glow { stroke: #f85848; stroke-width: 2; }
    .dot { fill: #8d99a5; pointer-events: none; }
    .legend { font-size: 0.75rem; color: #8d99a5; text-align: center; margin-top: 0.25rem; }
    .key.tgt .glow { fill: rgba(32, 200, 176, 0.1); stroke: #20c8b0; stroke-width: 1.4; stroke-dasharray: 4 3; animation: pulse 1.2s ease-in-out infinite; }
    .key.over .glow { fill: rgba(8, 96, 84, 0.78); stroke: #5fe3cf; stroke-width: 2; stroke-dasharray: none; animation: none; }
    .key.invalid .glow { stroke: #f85848; fill: rgba(248, 88, 72, 0.18); animation: none; }
    .key.sel .glow { fill: rgba(8, 96, 84, 0.88); stroke: #fff; stroke-width: 2.2; stroke-dasharray: none; filter: drop-shadow(0 0 4px #20c8b0); }
    .key:focus-visible .glow { stroke: #5fe3cf; stroke-width: 2.4; stroke-dasharray: none; filter: drop-shadow(0 0 3px #5fe3cf); }
    @keyframes pulse { 50% { stroke-opacity: 0.35; } }
    .screen-frame { fill: none; stroke: transparent; stroke-width: 3; pointer-events: none; }
    .screen-frame.tgt { stroke: #20c8b0; stroke-dasharray: 6 4; animation: pulse 1.2s ease-in-out infinite; }
    .grid line { stroke: rgba(255, 255, 255, 0.07); stroke-width: 0.6; }
    .wh { fill: transparent; stroke: none; cursor: grab; outline: none; }
    .wh:hover { stroke: rgba(255, 255, 255, 0.35); stroke-width: 1.2; }
    .wh.sel { stroke: #fff; stroke-width: 2.6; filter: drop-shadow(0 0 3px #20c8b0); }
    .wh:focus-visible { stroke: #5fe3cf; stroke-width: 2.2; }
    .wh.bad { stroke: #f85848; stroke-width: 1.8; stroke-dasharray: 4 2; }
    .mark { stroke: var(--sf); stroke-width: 1.2; cursor: pointer; }
    .mark.warn { fill: var(--ir); }
    .wh.tgt { stroke: rgba(32, 200, 176, 0.7); stroke-dasharray: 4 3; stroke-width: 1.4; }
    .wh.over { stroke: #5fe3cf; stroke-width: 2; fill: rgba(32, 200, 176, 0.18); stroke-dasharray: none; }
    .cell { fill: transparent; }
    .cell.over { fill: rgba(32, 200, 176, 0.25); stroke: #5fe3cf; stroke-width: 1.5; stroke-dasharray: 3 2; }
    .ghost { fill: rgba(32, 200, 176, 0.22); stroke: #20c8b0; stroke-width: 1.6; stroke-dasharray: 4 3; pointer-events: none; }
    .ghost.invalid { fill: rgba(248, 88, 72, 0.25); stroke: #f85848; }
    .handle { fill: #fff; stroke: #20c8b0; stroke-width: 1.6; cursor: nwse-resize; }
    .xl { fill: var(--tx, #14202a); font: 600 16.5px system-ui, sans-serif; }
    .xl.inh { fill: var(--tx3, #5f6c78); font-weight: 500; }
    .xline { stroke: var(--tx3, #5f6c78); stroke-width: 0.8; opacity: 0.55; }
    .xkey { fill: var(--tx, #14202a); font: 700 8.5px system-ui, sans-serif; opacity: 0.6; }
    @media (prefers-reduced-motion: reduce) { .key.tgt .glow, .screen-frame.tgt { animation: none; } }
  `;

  @property({ attribute: false }) host!: StudioHost;
  @property({ attribute: false }) assigner!: Assigner;
  @property({ type: Boolean, reflect: true }) fit = false;
  @state() private session: Session | null = null;
  @state() private avail = 0;
  private ro?: ResizeObserver;

  private get store() {
    return this.host.state.store;
  }
  private bound: EventTarget[] = [];
  private rerender = () => this.requestUpdate();
  protected override updated(): void {
    const now = this.host ? [this.host.state, this.host.state.store, this.assigner] : [];
    if (now.length === this.bound.length && now.every((t, i) => t === this.bound[i])) return;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = now;
    now.forEach((t) => t.addEventListener('change', this.rerender));
  }
  override connectedCallback(): void {
    super.connectedCallback();
    this.ro = new ResizeObserver((e) => {
      const w = Math.round(e[0]?.contentRect.width ?? 0);
      if (Math.abs(w - this.avail) > 2) this.avail = w;
    });
    this.ro.observe(this);
  }
  override disconnectedCallback(): void {
    this.ro?.disconnect();
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = [];
    super.disconnectedCallback();
  }

  // ------------------------------------------------------------ data for this render
  private get st() {
    return this.host.state;
  }
  private cfgPage(): Page | undefined {
    return this.store.config.pages?.[this.st.page];
  }
  private v2(): V2Page | null {
    const pp = this.st.previewOf(this.st.page);
    return pp ? (pp.wire_pages[this.st.wire]?.page ?? pp.page) : null;
  }
  /** Is config widget i drawn on the screen shown? (a promoted header label has no wire widget but is always drawn) */
  private onScreen(page: Page, i: number, map: number[]): boolean {
    return !this.multiScreen() || (map[i] ?? -1) >= 0 || i === promotedIndex(page);
  }
  private multiScreen(): boolean {
    return (this.st.previewOf(this.st.page)?.wire_pages.length ?? 0) > 1;
  }
  /** Overlay rectangle in screen pixels for config widget i. */
  private rectOf(page: Page, i: number, v2: V2Page | null, map: number[]) {
    const m = map[i] ?? -1;
    const w = v2?.widgets[m];
    if (v2 && w) return widgetRect(v2, w);
    const b = boxOf(page.widgets[i] as never);
    const g = gridOf(page);
    return widgetRect({ cols: g.cols, rows: g.rows, version: 2 } as V2Page, { ...b, y: b.y - g.shift });
  }
  /** Explicit `layout: fixed` pages split by config rows, so a config cell of a drawn row is a compiled cell; converted pages are regridded and have no such cells. */
  private fixedCells(page: Page): boolean {
    return this.multiScreen() && page.layout === 'fixed' && !isList(page);
  }
  /** Compiled pixel rows (y1..y2) of the config rows the shown screen draws. */
  private shownRows(page: Page): Map<number, { y1: number; y2: number }> {
    const v2 = this.v2();
    const map = wireMap(page, this.st.previewOf(this.st.page), this.st.wire);
    const rows = new Map<number, { y1: number; y2: number }>();
    page.widgets.forEach((w, i) => {
      if ((map[i] ?? -1) < 0) return;
      const r = this.rectOf(page, i, v2, map);
      const y = boxOf(w).y;
      const cur = rows.get(y);
      rows.set(y, { y1: Math.min(cur?.y1 ?? r.y1, r.y1), y2: Math.max(cur?.y2 ?? r.y2, r.y2) });
    });
    return rows;
  }
  private isFreeCell(page: Page, x: number, y: number): boolean {
    return !page.widgets.some((w) => {
      const b = boxOf(w);
      return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
    });
  }
  private cellRect(page: Page, x: number, y: number) {
    const row = this.fixedCells(page) ? this.shownRows(page).get(y) : undefined;
    if (row) {
      const cols = pageDims(page).cols;
      return { x1: Math.ceil((x * 240) / cols), x2: Math.ceil(((x + 1) * 240) / cols) - 1, y1: row.y1, y2: row.y2 };
    }
    const g = gridOf(page);
    return widgetRect({ cols: g.cols, rows: g.rows, version: 2 } as V2Page, { x, y: y - g.shift, w: 1, h: 1 });
  }

  // ------------------------------------------------------------ hit testing (drag targets, keyboard cursor)
  private screenEl(): SVGSVGElement | null {
    return this.renderRoot.querySelector('svg.scr');
  }
  /** Client point to screen pixels (0..240, 0..320), or null outside the display. */
  screenPoint(cx: number, cy: number, clamp = false): { x: number; y: number } | null {
    const el = this.screenEl();
    if (!el) return null;
    const r = el.getBoundingClientRect();
    if (!r.width) return null;
    const x = ((cx - r.left) * 240) / r.width;
    const y = ((cy - r.top) * 320) / r.height;
    if (!clamp && (x < 0 || y < 0 || x >= 240 || y >= 320)) return null;
    return { x, y };
  }
  private cfgCellAt(px: number, py: number): { x: number; y: number } | null {
    const page = this.cfgPage();
    if (!page || isList(page)) return null;
    if (this.multiScreen()) {
      if (!this.fixedCells(page)) return null;
      const cols = pageDims(page).cols;
      const x = Math.min(cols - 1, Math.floor((px * cols) / 240));
      for (const [y, r] of this.shownRows(page)) if (py >= r.y1 && py <= r.y2 && this.isFreeCell(page, x, y)) return { x, y };
      return null;
    }
    const g = gridOf(page);
    const c = cellAt({ cols: g.cols, rows: g.rows, version: 2, widgets: [] } as unknown as V2Page, px, py);
    return c ? { x: c.x, y: c.y + g.shift } : null;
  }
  /** Drop target under a client point: a hard key, a widget, or an empty cell. */
  targetAt(cx: number, cy: number): Target | null {
    const root = this.renderRoot as ShadowRoot;
    for (const el of root.elementsFromPoint(cx, cy)) {
      const k = (el as Element).closest?.('[data-key]')?.getAttribute('data-key');
      if (k) return { kind: 'key', name: k };
    }
    const p = this.screenPoint(cx, cy);
    const page = this.cfgPage();
    if (!p || !page) return null;
    if (isList(page)) return { kind: 'cell', page: this.st.page, x: 0, y: 0 };
    if (this.multiScreen()) {
      // the config grid is not what is drawn: hit-test the compiled screen first
      const v2 = this.v2();
      const map = wireMap(page, this.st.previewOf(this.st.page), this.st.wire);
      const index = !v2 ? -1 : map.findIndex((m) => {
        const w = m >= 0 ? v2.widgets[m] : undefined;
        if (!w) return false;
        const r = widgetRect(v2, w);
        return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
      });
      if (index >= 0) return { kind: 'widget', page: this.st.page, index };
      const cell = this.cfgCellAt(p.x, p.y); // only on explicit fixed pages
      return cell ? { kind: 'cell', page: this.st.page, x: cell.x, y: cell.y } : null;
    }
    const c = this.cfgCellAt(p.x, p.y);
    if (!c) return null;
    const index = page.widgets.findIndex((w) => {
      const b = boxOf(w);
      return c.x >= b.x && c.x < b.x + b.w && c.y >= b.y && c.y < b.y + b.h;
    });
    return index >= 0 ? { kind: 'widget', page: this.st.page, index } : { kind: 'cell', page: this.st.page, x: c.x, y: c.y };
  }
  /** Keyboard targets with centres in one coordinate space (the remote's viewBox). */
  spots(): Spot[] {
    const out: Spot[] = KEYS.map((g) => ({ id: `k:${g.name}`, target: { kind: 'key', name: g.name } as Target, x: g.x + g.w / 2, y: g.y + g.h / 2 }));
    const page = this.cfgPage();
    if (!page) return out;
    if (isList(page)) return [...out, { id: `c:${this.st.page}:0:0`, target: { kind: 'cell', page: this.st.page, x: 0, y: 0 }, x: SCREEN.x + 120, y: SCREEN.y + 160 }];
    const v2 = this.v2();
    const map = wireMap(page, this.st.previewOf(this.st.page), this.st.wire);
    const { cols, rows } = pageDims(page);
    const covered = new Set<string>();
    page.widgets.forEach((w, i) => {
      if (!this.onScreen(page, i, map)) return;
      const b = boxOf(w);
      for (let y = b.y; y < b.y + b.h; y++) for (let x = b.x; x < b.x + b.w; x++) covered.add(`${x},${y}`);
      const r = this.rectOf(page, i, v2, map);
      out.push({ id: `w:${this.st.page}:${i}`, target: { kind: 'widget', page: this.st.page, index: i }, x: SCREEN.x + (r.x1 + r.x2) / 2, y: SCREEN.y + (r.y1 + r.y2) / 2 });
    });
    if (this.multiScreen()) {
      // explicit fixed pages: the free cells of the rows this screen draws; converted pages have no compiled cells
      if (!this.fixedCells(page)) return out;
      for (const [y] of this.shownRows(page))
        for (let x = 0; x < cols; x++)
          if (this.isFreeCell(page, x, y)) {
            const r = this.cellRect(page, x, y);
            out.push({ id: `c:${this.st.page}:${x}:${y}`, target: { kind: 'cell', page: this.st.page, x, y }, x: SCREEN.x + (r.x1 + r.x2) / 2, y: SCREEN.y + (r.y1 + r.y2) / 2 });
          }
      return out;
    }
    for (let y = 0; y < rows; y++)
      for (let x = 0; x < cols; x++)
        if (!covered.has(`${x},${y}`)) {
          const r = this.cellRect(page, x, y);
          out.push({ id: `c:${this.st.page}:${x}:${y}`, target: { kind: 'cell', page: this.st.page, x, y }, x: SCREEN.x + (r.x1 + r.x2) / 2, y: SCREEN.y + (r.y1 + r.y2) / 2 });
        }
    return out;
  }

  // ------------------------------------------------------------ screen pointer editing
  private onWidgetDown(e: PointerEvent, page: number, index: number, resize = false) {
    if (e.button !== 0) return;
    e.stopPropagation();
    if (this.assigner.phase.name === 'picked') {
      this.host.activateWidget(page, index);
      return;
    }
    const sl = this.st.sel;
    const wasSel = sl?.kind === 'widget' && sl.page === page && sl.index === index;
    this.host.activateWidget(page, index);
    const cfg = this.cfgPage();
    const w = cfg?.widgets[index];
    if (!cfg || !w || isList(cfg)) return;
    const p = this.screenPoint(e.clientX, e.clientY, true);
    if (!p) return;
    (e.currentTarget as Element).setPointerCapture?.(e.pointerId);
    const box = boxOf(w);
    this.session = { kind: resize ? 'resize' : 'move', page, index, startX: p.x, startY: p.y, box, ghost: box, valid: true, moved: false, id: e.pointerId, wasSel };
  }
  private onMove = (e: PointerEvent) => {
    const s = this.session;
    const cfg = this.cfgPage();
    if (!s || e.pointerId !== s.id || !cfg) return;
    const p = this.screenPoint(e.clientX, e.clientY, true);
    if (!p) return;
    const g = gridOf(cfg);
    const cw = 240 / g.cols;
    const ch = 288 / g.rows;
    const dx = Math.round((p.x - s.startX) / cw);
    const dy = Math.round((p.y - s.startY) / ch);
    const ghost = s.kind === 'move' ? { ...s.box, x: s.box.x + dx, y: s.box.y + dy } : { ...s.box, w: s.box.w + dx, h: s.box.h + dy };
    const moved = s.moved || Math.abs(p.x - s.startX) + Math.abs(p.y - s.startY) > 3;
    this.session = { ...s, ghost, valid: fits(cfg, ghost, s.index), moved };
  };
  private onUp = (e: PointerEvent) => {
    const s = this.session;
    if (!s || e.pointerId !== s.id) return;
    this.session = null;
    if (s.moved && s.valid && JSON.stringify(s.ghost) !== JSON.stringify(s.box)) this.host.setBox(s.page, s.index, s.ghost);
    else if (s.moved && !s.valid) this.host.state.say('That spot is taken or off the grid');
    else if (!s.moved && s.wasSel && s.kind === 'move') this.host.deselect();
  };
  /** a click on the remote's own body (not a key, the screen or a plate) clears the selection */
  private onBackdrop = (e: MouseEvent) => {
    if (this.assigner.phase.name !== 'idle') return;
    const t = e.target as Element;
    if (t.closest('[data-key], .scr, .plate')) return;
    this.host.deselect();
  };
  private onScreenDown(e: PointerEvent) {
    if (e.button !== 0) return;
    const p = this.screenPoint(e.clientX, e.clientY);
    const c = p && this.cfgCellAt(p.x, p.y);
    if (c) this.host.activateCell(this.st.page, c.x, c.y);
    else if (p && this.cfgPage() && isList(this.cfgPage() as Page)) this.host.activateCell(this.st.page, 0, 0);
  }
  private onScreenKey(e: KeyboardEvent) {
    const page = this.cfgPage();
    if (!page) return;
    const sel = this.st.sel;
    if (e.key.toLowerCase() === 'n' && !e.ctrlKey && !e.metaKey) {
      e.preventDefault();
      this.host.addWidgetFree();
      return;
    }
    if (sel?.kind !== 'widget') return;
    const dir: Record<string, [number, number]> = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
    const d = dir[e.key];
    if (d && this.assigner.phase.name === 'idle') {
      e.preventDefault();
      this.host.nudge(sel.page, sel.index, d[0], d[1], e.shiftKey);
    } else if ((e.key === 'Delete' || e.key === 'Backspace') && !(e.target as HTMLElement).closest?.('input,textarea')) {
      e.preventDefault();
      this.host.removeWidget(sel.page, sel.index);
    } else if (e.key === 'Enter' || e.key === ' ') {
      if (this.assigner.phase.name === 'idle') {
        e.preventDefault();
        this.host.openPopover();
      }
    }
  }

  // ------------------------------------------------------------ rendering
  private canTarget(kind: 'key' | 'screen'): boolean {
    const a = this.assigner.phase;
    if (a.name === 'idle') return false;
    const s: Source = a.source;
    return kind === 'key' ? s.kind !== 'widget' : s.kind !== 'default';
  }
  private keyState(row: KeymapRow | undefined): string[] {
    const layer = this.st.layer;
    const cls: string[] = [];
    const none = row?.action?.kind === 'none';
    if (!row || row.source === 'none') cls.push('none');
    else if (none) cls.push('ex');
    else if (layer === DEFAULT_LAYER || row.source === 'activity') cls.push('ov');
    return cls;
  }
  private keyLabel(g: KeyGeom, row: KeymapRow | undefined): string {
    const layer = this.st.layerName(this.st.layer);
    const what = !row || row.source === 'none' ? 'does nothing' : row.action?.kind === 'none' ? 'switched off' : row.action ? this.st.actionLabel(row.action) : undefined;
    const how = !row ? '' : this.st.layer === DEFAULT_LAYER ? (row.source === 'none' ? '' : ', assigned') : row.source === 'activity' ? ', override' : row.source === 'default' ? ', from Default' : '';
    return `${keyTitle(g.name)}, ${layer} Layer: ${what}${how}`;
  }
  private keyGlyph(g: KeyGeom): TemplateResult | typeof nothing {
    const cx = g.x + g.w / 2;
    const cy = g.y + g.h / 2;
    const arrow = (dx: number, dy: number, d: string) => svg`<path class="gl dir" d=${d} transform="translate(${DPAD.x + dx * DPAD.r} ${DPAD.y + dy * DPAD.r})"></path>`;
    const sign = (x: number, y: number, plus: boolean) => svg`<path class="gs mk" d=${`M${x - 3.5} ${y}h7${plus ? `M${x} ${y - 3.5}v7` : ''}`}></path>`;
    switch (g.name) {
      case 'up': return arrow(0, -0.6, 'M0 -6l7 9h-14z');
      case 'down': return arrow(0, 0.6, 'M0 6l7 -9h-14z');
      case 'left': return arrow(-0.6, 0, 'M-6 0l9 -7v14z');
      case 'right': return arrow(0.6, 0, 'M6 0l-9 -7v14z');
      case 'volume_up': return sign(g.x + 16, g.y + 13, true);
      case 'volume_down': return sign(g.x + 16, g.y + g.h - 13, false);
      case 'channel_up': return sign(g.x + g.w - 16, g.y + 13, true);
      case 'channel_down': return sign(g.x + g.w - 16, g.y + g.h - 13, false);
      case 'volume_mute': return svg`<path class="gs" d=${`M${cx - 7} ${cy - 2}h3l4 -4v12l-4 -4h-3zM${cx - 9} ${cy - 8}l16 16`}></path>`;
      case 'prev_channel': return svg`<path class="gs" d=${`M${cx - 9} ${cy - 3}l5 -5m-5 5l5 5m-5 -5h13a6 6 0 0 1 0 12h-7`}></path>`;
      // a spotlight pointing down: a tapered cone with two bands and a pointed tip
      case PHYSICAL_KEYS.lightUpper:
      case PHYSICAL_KEYS.lightLower: return svg`<path class="gs" d=${`M${cx - 6} ${cy - 9}h12l-3.5 12.5l-2.5 4.5l-2.5 -4.5zM${cx - 4.6} ${cy - 4}h9.2M${cx - 3.5} ${cy}h7`}></path>`;
      // a US wall outlet: a squircle with two slots and a ground hole
      case PHYSICAL_KEYS.socketUpper:
      case PHYSICAL_KEYS.socketLower: return svg`<rect class="gs" x=${cx - 11} y=${cy - 9.5} width="22" height="19" rx="7"></rect><path class="gs" d=${`M${cx - 3.5} ${cy - 5}v4M${cx + 3.5} ${cy - 5}v4`}></path><circle class="gs" cx=${cx} cy=${cy + 4.5} r="1.3"></circle>`;
      case 'play': return svg`<path class="gs" d=${`M${cx - 4} ${cy - 6.5}l10 6.5l-10 6.5z`}></path>`;
      case 'pause': return svg`<path class="gs" d=${`M${cx - 3.5} ${cy - 5}v10M${cx + 3.5} ${cy - 5}v10`}></path>`;
      case 'stop': return svg`<rect class="gs" x=${cx - 5} y=${cy - 5} width="10" height="10"></rect>`;
      case 'record': return svg`<circle class="gs rec" cx=${cx} cy=${cy} r="5"></circle>`;
      case 'rewind': return svg`<path class="gs" d=${`M${cx - 12} ${cy - 5}v10M${cx - 2} ${cy - 5}l-8 5l8 5zM${cx + 10} ${cy - 6}l-9 6l9 6z`}></path>`;
      case 'fast_forward': return svg`<path class="gs" d=${`M${cx + 12} ${cy - 5}v10M${cx + 2} ${cy - 5}l8 5l-8 5zM${cx - 10} ${cy - 6}l9 6l-9 6z`}></path>`;
      default: return nothing;
    }
  }
  private keyEl(g: KeyGeom, row: KeymapRow | undefined, errs: Set<string>): TemplateResult {
    const a = this.assigner.phase;
    const cur = this.assigner.cursor;
    const tgt = this.canTarget('key');
    const cls = ['key', `g-${g.group}`, JOINED.has(g.name) ? 'jn' : '', PLATED.has(g.name) ? 'jp' : '', g.color ? 'colour' : '', ...this.keyState(row)];
    const sel = this.st.sel?.kind === 'key' && this.st.sel.name === g.name;
    if (sel) cls.push('sel');
    if (errs.has(g.name)) cls.push('bad');
    if (tgt) cls.push('tgt');
    if (tgt && cur?.kind === 'key' && cur.name === g.name) cls.push('over');
    if (a.name === 'dragging' && a.over?.kind === 'key' && a.over.name === g.name) cls.push('over');
    const path = keyPath(g);
    const inh = this.st.layer !== DEFAULT_LAYER && row?.source === 'default';
    const label = this.keyLabel(g, row);
    return svg`<g class=${cls.filter(Boolean).join(' ')} data-key=${g.name} role="button" tabindex="0" aria-label=${label} aria-pressed=${sel ? 'true' : 'false'}
        @click=${() => this.host.activateKey(g.name)}
        @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); this.host.activateKey(g.name); } }}>
      <title>${label}</title>
      <path class="cap" d=${path}></path>
      ${g.color ? svg`<path class="tint" d=${tintPath(g)} stroke=${g.color}></path>` : nothing}
      <path class="glow" d=${path}></path>
      ${this.keyGlyph(g)}
      ${!g.label ? nothing : svg`<text class="lab" x=${g.lx ?? g.x + g.w / 2} y=${g.ly ?? g.y + g.h / 2}>${g.label}</text>`}
      ${inh ? ((): TemplateResult => { const [dx, dy] = dotAnchor(g); return svg`<circle class="dot" cx=${dx} cy=${dy} r="2.6"><title>Inherited from Default</title></circle>`; })() : nothing}
    </g>`;
  }

  private screenOverlay(): TemplateResult {
    const page = this.cfgPage();
    const v2 = this.v2();
    if (!page) return svg``;
    const st = this.st;
    const map = wireMap(page, this.st.previewOf(this.st.page), this.st.wire);
    const g = gridOf(page);
    const list = isList(page);
    const showGrid = !list && (st.mode === 'screen' || this.assigner.active || st.sel?.kind === 'widget' || !!this.session);
    const cur = this.assigner.cursor;
    const dragOver = this.assigner.phase.name === 'dragging' ? this.assigner.phase.over : null;
    const over = dragOver ?? cur;
    const sc = this.canTarget('screen');
    const errs = new Set<number>();
    for (const er of this.store.errors) {
      const t = errorTarget(er.path);
      if (t && (t.kind === 'widget') && t.page === st.page) errs.add(t.index);
    }
    const marks = widgetIssues(collectIssues(this.store.errors, this.store.warnings, this.store.checks, this.store.config), st.page);
    const lines: TemplateResult[] = [];
    if (showGrid) {
      const edge = (i: number, n: number, total: number) => Math.ceil((i * total) / n);
      for (let c = 1; c < g.cols; c++) lines.push(svg`<line x1=${edge(c, g.cols, 240)} y1="32" x2=${edge(c, g.cols, 240)} y2="320"></line>`);
      for (let r = 1; r < g.rows; r++) lines.push(svg`<line x1="0" y1=${32 + edge(r, g.rows, 288)} x2="240" y2=${32 + edge(r, g.rows, 288)}></line>`);
    }
    const sel = st.sel?.kind === 'widget' && st.sel.page === st.page ? st.sel.index : -1;
    const widgets = page.widgets.map((w, i) => {
      if (!this.onScreen(page, i, map)) return nothing;
      const r = this.rectOf(page, i, v2, map);
      const cls = ['wh'];
      if (i === sel) cls.push('sel');
      if (errs.has(i)) cls.push('bad');
      if (sc && w.type !== 'label') cls.push('tgt');
      if (over?.kind === 'widget' && over.page === st.page && over.index === i) cls.push('over');
      const label = `${w.type} ${w.label ?? ''}${errs.has(i) ? ', has an error' : ''}${marks.has(i) ? ', has a warning' : ''}`.trim();
      return svg`<rect class=${cls.join(' ')} data-widget=${i} x=${r.x1} y=${r.y1} width=${r.x2 - r.x1 + 1} height=${r.y2 - r.y1 + 1} rx="10"
        role="button" tabindex="0" aria-label=${label} aria-pressed=${i === sel ? 'true' : 'false'}
        @pointerdown=${(e: PointerEvent) => this.onWidgetDown(e, st.page, i)}
        @focus=${() => { if (!(st.sel?.kind === 'widget' && st.sel.index === i && st.sel.page === st.page)) this.host.activateWidget(st.page, i); }}></rect>`;
    });
    const markers = [...marks].filter(([i]) => !v2 || this.onScreen(page, i, map)).map(([i, is]) => {
      const r = this.rectOf(page, i, v2, map);
      return svg`<circle class="mark warn" cx=${r.x2 - 3} cy=${r.y1 + 4} r="5" @pointerdown=${(e: PointerEvent) => this.onWidgetDown(e, st.page, i)}><title>${is.text}</title></circle>`;
    });
    let handle: TemplateResult | typeof nothing = nothing;
    if (sel >= 0 && !list && !this.assigner.active && this.onScreen(page, sel, map)) {
      const r = this.rectOf(page, sel, v2, map);
      handle = svg`<rect class="handle" x=${r.x2 - 7} y=${r.y2 - 7} width="12" height="12" rx="3" @pointerdown=${(e: PointerEvent) => this.onWidgetDown(e, st.page, sel, true)}></rect>`;
    }
    let cell: TemplateResult | typeof nothing = nothing;
    if (over?.kind === 'cell' && over.page === st.page && !list) {
      const r = this.cellRect(page, over.x, over.y);
      cell = svg`<rect class="cell over" x=${r.x1} y=${r.y1} width=${r.x2 - r.x1 + 1} height=${r.y2 - r.y1 + 1} rx="10"></rect>`;
    }
    let ghost: TemplateResult | typeof nothing = nothing;
    const s = this.session;
    if (s?.moved) {
      const b = s.ghost;
      const at = this.multiScreen() ? this.rectOf(page, s.index, v2, map) : null;
      // a multi-screen page is drawn in compiled geometry: move the drawn widget by the cell delta instead of using the config grid's rows
      const r = at ? { x1: at.x1 + (b.x - s.box.x) * (240 / g.cols), y1: at.y1 + (b.y - s.box.y) * (288 / g.rows), x2: at.x2 + (b.x + b.w - s.box.x - s.box.w) * (240 / g.cols), y2: at.y2 + (b.y + b.h - s.box.y - s.box.h) * (288 / g.rows) } : widgetRect({ cols: g.cols, rows: g.rows, version: 2 } as V2Page, { x: Math.max(0, Math.min(g.cols - 1, b.x)), y: Math.max(0, Math.min(g.rows - 1, b.y - g.shift)), w: Math.max(1, Math.min(b.w, g.cols - Math.max(0, b.x))), h: Math.max(1, Math.min(b.h, g.rows - Math.max(0, b.y - g.shift))) });
      ghost = svg`<rect class=${s.valid ? 'ghost' : 'ghost invalid'} x=${r.x1} y=${r.y1} width=${r.x2 - r.x1 + 1} height=${r.y2 - r.y1 + 1} rx="10"></rect>`;
    }
    return svg`<g class="grid">${lines}</g>${cell}<g role="application" aria-label="Remote screen editor" @keydown=${(e: KeyboardEvent) => this.onScreenKey(e)}>${widgets}</g>${markers}${handle}${ghost}`;
  }

  private xray(rows: KeymapRow[]): TemplateResult {
    const labels = xrayLabels(rows.map((r) => ({ key: r.key, text: r.action && r.action.kind !== 'none' ? this.st.actionLabel(r.action) : r.action ? 'Does nothing' : '' })));
    const inherit = new Map(rows.map((r) => [r.key, r.source === 'default' && this.st.layer !== DEFAULT_LAYER]));
    return svg`<g aria-hidden="true">${labels.map((l) => {
      const left = l.side === 'left';
      return svg`<line class="xline" x1=${l.x1} y1=${l.y1} x2=${left ? l.x + 6 : l.x - 6} y2=${l.y}></line>
        <text class=${inherit.get(l.key) ? 'xl inh' : 'xl'} x=${l.x} y=${l.y + 6} text-anchor=${left ? 'end' : 'start'}>${clip(l.text) === l.text ? nothing : svg`<title>${l.text}</title>`}${clip(l.text)}</text>`;
    })}</g>`;
  }

  protected override render() {
    if (!this.host) return nothing;
    const st = this.st;
    const rows = st.rows();
    const byKey = new Map(rows.map((r) => [r.key, r]));
    const v2 = this.v2();
    const errKeys = new Set<string>();
    for (const er of this.store.errors) {
      const t = errorTarget(er.path);
      if (t?.kind === 'key' && (t.layer === st.layer || t.layer === `#${(this.store.config.activities ?? []).findIndex((a) => a.id === st.layer)}`)) errKeys.add(t.key);
    }
    const screenMode = st.mode === 'screen';
    const pad = st.xray && !screenMode ? XRAY_PAD : 0;
    const vb = screenMode ? { x: GLASS.x - 6, y: GLASS.y - 6, w: GLASS.w + 12, h: GLASS.h + 12 } : { x: -pad, y: 0, w: VIEW.w + pad * 2, h: VIEW.h };
    // pixels per viewBox unit: Screen mode uses an integer scale that fits; the whole remote uses its zoom
    const fitScale = this.avail > 0 ? Math.max(1, Math.floor(this.avail / vb.w)) : 1;
    const z = screenMode ? Math.min(st.scale, fitScale) : st.zoom === '2' ? 2 : st.zoom === '1' ? 1 : 0;
    const dims = z ? { width: vb.w * z, height: vb.h * z } : { width: undefined, height: undefined };
    this.toggleAttribute('fit', !z);
    const frame = this.assigner.phase.name !== 'idle' && this.canTarget('screen');
    const frameOver = (this.assigner.cursor ?? (this.assigner.phase.name === 'dragging' ? this.assigner.phase.over : null))?.kind;
    void frameOver;
    return html`<svg class="remote" viewBox="${vb.x} ${vb.y} ${vb.w} ${vb.h}" width=${dims.width ?? nothing} height=${dims.height ?? nothing} role="group" aria-label="Harmony Elite remote, ${st.layerName(st.layer)} layer" preserveAspectRatio="xMidYMid meet"
      @pointermove=${this.onMove} @pointerup=${this.onUp} @pointercancel=${this.onUp} @click=${this.onBackdrop}>
      <defs>
        <linearGradient id="bodyg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#23282c"></stop><stop offset="0.35" stop-color="#101315"></stop><stop offset="1" stop-color="#090b0c"></stop></linearGradient>
        <linearGradient id="keyg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4f565d"></stop><stop offset="1" stop-color="#3d4349"></stop></linearGradient>
        <linearGradient id="keyh" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5f666e"></stop><stop offset="1" stop-color="#4b5258"></stop></linearGradient>
      </defs>
      ${st.xray && !screenMode ? this.xray(rows) : nothing}
      <path class="body" d=${bodyPath()}></path>
      <path class="grip" d=${gripPath()}></path>
      <path class="seam" d=${`M${BODY.x + 1} ${GRIP_Y}h${BODY.w - 2}`}></path>
      <rect class="glass" x=${GLASS.x} y=${GLASS.y} width=${GLASS.w} height=${GLASS.h} rx=${GLASS.r}></rect>
      <svg class="scr" x=${SCREEN.x} y=${SCREEN.y} width=${SCREEN.w} height=${SCREEN.h} viewBox="0 0 240 320" @pointerdown=${(e: PointerEvent) => this.onScreenDown(e)}>
        ${v2 ? pageGroup(v2, { activeAct: activityIndex(this.store.config, st.layer), theme: activeTheme(this.store.config).theme }) : svg`<rect width="240" height="320" fill="#101820"></rect><text x="120" y="164" font-size="13" fill="#687888" text-anchor="middle">${this.store.pages.length || !this.store.loaded ? 'No preview for this page' : 'Add a page to see the screen'}</text>`}
        ${this.screenOverlay()}
      </svg>
      <rect class=${frame ? 'screen-frame tgt' : 'screen-frame'} x=${SCREEN.x - 2} y=${SCREEN.y - 2} width=${SCREEN.w + 4} height=${SCREEN.h + 4} rx="4"></rect>
      ${PLATES.map((d) => svg`<path class="plate" d=${d}></path>`)}
      ${KEYS.map((g) => this.keyEl(g, byKey.get(g.name), errKeys))}
      ${PRINTED.map((p) => { const n = Math.max(p.text.length, p.sub?.length ?? 0) * 7 + 8; return svg`<rect class="pbk" x=${p.x - n / 2} y=${p.sub ? p.y - 17 : p.y - 9} width=${n} height=${p.sub ? 34 : 18} rx="4"></rect><text class="pr" x=${p.x} y=${p.sub ? p.y - 8 : p.y}>${p.text}</text>${p.sub ? svg`<path class="prl" d=${`M${p.x - 8} ${p.y}h16`}></path><text class="pr" x=${p.x} y=${p.y + 8}>${p.sub}</text>` : nothing}`; })}
    </svg>${st.layer !== DEFAULT_LAYER ? html`<div class="legend"><svg width="8" height="8" viewBox="0 0 8 8" aria-hidden="true"><circle cx="4" cy="4" r="2.6" fill="#8d99a5"></circle></svg> Inherited from Default</div>` : nothing}`;
  }
}
