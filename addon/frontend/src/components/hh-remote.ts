import { LitElement, css, html, nothing, svg, type TemplateResult } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { KeymapRow, V2Page } from '../lib/api';
import type { Assigner, Spot } from '../lib/assigner';
import { cellAt, pageGroup, widgetRect } from '../lib/page-renderer';
import { BODY, GLASS, KEYS, SCREEN, VIEW, keyPath, keyTitle, type KeyGeom } from '../lib/remote-layout';
import type { StudioHost } from '../lib/studio-state';
import { DEFAULT_LAYER, activityIndex, boxOf, errorTarget, fits, gridOf, isList, pageDims, wireMap, type Box, type Source, type Target } from '../lib/studio-model';
import { XRAY_PAD, xrayLabels } from '../lib/xray';
import type { Page } from '../lib/model';

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
}
const tid = (t: Target): string => (t.kind === 'key' ? `k:${t.name}` : t.kind === 'cell' ? `c:${t.page}:${t.x}:${t.y}` : `w:${t.page}:${t.index}`);
export { tid };

/** The Harmony Elite drawing with the live screen, hard keys as buttons, and the screen editor overlay. */
@customElement('hh-remote')
export class HhRemote extends LitElement {
  static override styles = css`
    :host { display: block; min-width: 0; }
    :host([fit]) { height: 100%; min-height: 360px; }
    svg.remote { display: block; margin: 0 auto; max-width: none; touch-action: pan-x pan-y pinch-zoom; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
    :host([fit]) svg.remote { width: 100%; height: 100%; }
    .body { fill: url(#bodyg); stroke: #0a0d10; stroke-width: 2; }
    .rim { fill: none; stroke: rgba(255, 255, 255, 0.07); stroke-width: 1.5; }
    .glass { fill: #05080b; stroke: #1b2229; stroke-width: 2; }
    .hint { fill: #55616c; font: 600 8px system-ui, sans-serif; letter-spacing: 1px; text-transform: uppercase; }
    .key { cursor: pointer; outline: none; }
    .cap { fill: url(#keyg); stroke: #11161a; stroke-width: 1; transition: fill 140ms ease-out; }
    .key.colour .cap { stroke: rgba(0, 0, 0, 0.5); }
    .glow { fill: none; stroke: none; pointer-events: none; transition: fill 140ms ease-out, stroke 140ms ease-out; }
    .lab { fill: #aab5bf; font: 700 11px system-ui, sans-serif; text-anchor: middle; pointer-events: none; }
    .key.colour .lab { display: none; }
    .key:hover .cap { fill: url(#keyh); }
    .key.ov .glow { fill: rgba(32, 200, 176, 0.28); stroke: #20c8b0; stroke-width: 1.6; }
    .key.none .glow { fill: rgba(5, 8, 11, 0.55); }
    .key.ex .glow { fill: rgba(5, 8, 11, 0.4); stroke: #687888; stroke-width: 1; stroke-dasharray: 3 2; }
    .key.bad .glow { stroke: #f85848; stroke-width: 2; }
    .dot { fill: #8d99a5; pointer-events: none; }
    .key.tgt .glow { fill: rgba(32, 200, 176, 0.1); stroke: #20c8b0; stroke-width: 1.4; stroke-dasharray: 4 3; animation: pulse 1.2s ease-in-out infinite; }
    .key.over .glow { fill: rgba(32, 200, 176, 0.38); stroke: #5fe3cf; stroke-width: 2; stroke-dasharray: none; animation: none; }
    .key.invalid .glow { stroke: #f85848; fill: rgba(248, 88, 72, 0.18); animation: none; }
    .key.sel .glow { stroke: #fff; stroke-width: 2.2; stroke-dasharray: none; filter: drop-shadow(0 0 4px #20c8b0); }
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
  private wireOffset(): number {
    const pp = this.st.previewOf(this.st.page);
    return (pp?.wire_pages ?? []).slice(0, this.st.wire).reduce((n, w) => n + w.widgets, 0);
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
  private cellRect(page: Page, x: number, y: number) {
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
    const map = wireMap(page, v2?.widgets.length ?? 0, this.wireOffset());
    const { cols, rows } = pageDims(page);
    const covered = new Set<string>();
    page.widgets.forEach((w, i) => {
      const b = boxOf(w);
      for (let y = b.y; y < b.y + b.h; y++) for (let x = b.x; x < b.x + b.w; x++) covered.add(`${x},${y}`);
      const r = this.rectOf(page, i, v2, map);
      out.push({ id: `w:${this.st.page}:${i}`, target: { kind: 'widget', page: this.st.page, index: i }, x: SCREEN.x + (r.x1 + r.x2) / 2, y: SCREEN.y + (r.y1 + r.y2) / 2 });
    });
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
    this.host.activateWidget(page, index);
    const cfg = this.cfgPage();
    const w = cfg?.widgets[index];
    if (!cfg || !w || isList(cfg)) return;
    const p = this.screenPoint(e.clientX, e.clientY, true);
    if (!p) return;
    (e.currentTarget as Element).setPointerCapture?.(e.pointerId);
    const box = boxOf(w);
    this.session = { kind: resize ? 'resize' : 'move', page, index, startX: p.x, startY: p.y, box, ghost: box, valid: true, moved: false, id: e.pointerId };
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
    const what = !row || row.source === 'none' ? 'does nothing' : row.action?.kind === 'none' ? 'switched off' : row.action?.label;
    const how = !row ? '' : this.st.layer === DEFAULT_LAYER ? (row.source === 'none' ? '' : ', assigned') : row.source === 'activity' ? ', override' : row.source === 'default' ? ', from Default' : '';
    return `${keyTitle(g.name)}, ${layer} layer: ${what}${how}`;
  }
  private keyGlyph(g: KeyGeom): TemplateResult | typeof nothing {
    const cx = g.x + g.w / 2;
    const cy = g.y + g.h / 2;
    const tri = (d: string) => svg`<path d=${d} fill="#9aa6b1" pointer-events="none"></path>`;
    switch (g.name) {
      case 'up': return tri(`M${cx} ${cy - 6}l7 9h-14z`);
      case 'down': return tri(`M${cx} ${cy + 6}l7 -9h-14z`);
      case 'left': return tri(`M${cx - 6} ${cy}l9 -7v14z`);
      case 'right': return tri(`M${cx + 6} ${cy}l-9 -7v14z`);
      default: return nothing;
    }
  }
  private keyEl(g: KeyGeom, row: KeymapRow | undefined, errs: Set<string>): TemplateResult {
    const a = this.assigner.phase;
    const cur = this.assigner.cursor;
    const tgt = this.canTarget('key');
    const cls = ['key', g.color ? 'colour' : '', ...this.keyState(row)];
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
      <path class="cap" d=${path} fill=${g.color ?? ''}></path>
      <path class="glow" d=${path}></path>
      ${this.keyGlyph(g)}
      ${g.shape.startsWith('wedge') ? nothing : svg`<text class="lab" x=${g.x + g.w / 2} y=${g.y + g.h / 2 + 3.4}>${g.label}</text>`}
      ${inh ? svg`<circle class="dot" cx=${g.x + g.w - 8} cy=${g.y + 8} r="2.6"></circle>` : nothing}
    </g>`;
  }

  private screenOverlay(): TemplateResult {
    const page = this.cfgPage();
    const v2 = this.v2();
    if (!page) return svg``;
    const st = this.st;
    const map = wireMap(page, v2?.widgets.length ?? 0, this.wireOffset());
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
    const lines: TemplateResult[] = [];
    if (showGrid) {
      const edge = (i: number, n: number, total: number) => Math.ceil((i * total) / n);
      for (let c = 1; c < g.cols; c++) lines.push(svg`<line x1=${edge(c, g.cols, 240)} y1="32" x2=${edge(c, g.cols, 240)} y2="320"></line>`);
      for (let r = 1; r < g.rows; r++) lines.push(svg`<line x1="0" y1=${32 + edge(r, g.rows, 288)} x2="240" y2=${32 + edge(r, g.rows, 288)}></line>`);
    }
    const sel = st.sel?.kind === 'widget' && st.sel.page === st.page ? st.sel.index : -1;
    const widgets = page.widgets.map((w, i) => {
      const r = this.rectOf(page, i, v2, map);
      const cls = ['wh'];
      if (i === sel) cls.push('sel');
      if (errs.has(i)) cls.push('bad');
      if (sc && w.type !== 'label') cls.push('tgt');
      if (over?.kind === 'widget' && over.page === st.page && over.index === i) cls.push('over');
      const label = `${w.type} ${w.label ?? ''}${errs.has(i) ? ', has an error' : ''}`.trim();
      return svg`<rect class=${cls.join(' ')} data-widget=${i} x=${r.x1} y=${r.y1} width=${r.x2 - r.x1 + 1} height=${r.y2 - r.y1 + 1} rx="10"
        role="button" tabindex="0" aria-label=${label} aria-pressed=${i === sel ? 'true' : 'false'}
        @pointerdown=${(e: PointerEvent) => this.onWidgetDown(e, st.page, i)}
        @focus=${() => { if (!(st.sel?.kind === 'widget' && st.sel.index === i && st.sel.page === st.page)) this.host.activateWidget(st.page, i); }}></rect>`;
    });
    let handle: TemplateResult | typeof nothing = nothing;
    if (sel >= 0 && !list && !this.assigner.active) {
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
      const r = widgetRect({ cols: g.cols, rows: g.rows, version: 2 } as V2Page, { x: Math.max(0, Math.min(g.cols - 1, b.x)), y: Math.max(0, Math.min(g.rows - 1, b.y - g.shift)), w: Math.max(1, Math.min(b.w, g.cols - Math.max(0, b.x))), h: Math.max(1, Math.min(b.h, g.rows - Math.max(0, b.y - g.shift))) });
      ghost = svg`<rect class=${s.valid ? 'ghost' : 'ghost invalid'} x=${r.x1} y=${r.y1} width=${r.x2 - r.x1 + 1} height=${r.y2 - r.y1 + 1} rx="10"></rect>`;
    }
    return svg`<g class="grid">${lines}</g>${cell}<g role="application" aria-label="Remote screen editor" @keydown=${(e: KeyboardEvent) => this.onScreenKey(e)}>${widgets}</g>${handle}${ghost}`;
  }

  private xray(rows: KeymapRow[]): TemplateResult {
    const labels = xrayLabels(rows.map((r) => ({ key: r.key, text: r.action && r.action.kind !== 'none' ? String(r.action.label) : r.action ? 'Does nothing' : '' })));
    const inherit = new Map(rows.map((r) => [r.key, r.source === 'default' && this.st.layer !== DEFAULT_LAYER]));
    return svg`<g aria-hidden="true">${labels.map((l) => {
      const left = l.side === 'left';
      return svg`<line class="xline" x1=${l.x1} y1=${l.y1} x2=${left ? l.x + 6 : l.x - 6} y2=${l.y}></line>
        <text class=${inherit.get(l.key) ? 'xl inh' : 'xl'} x=${l.x} y=${l.y + 6} text-anchor=${left ? 'end' : 'start'}>${l.text}</text>`;
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
    const vb = screenMode ? { x: 14, y: 36, w: 272, h: 358 } : { x: -pad, y: 0, w: VIEW.w + pad * 2, h: VIEW.h };
    // pixels per viewBox unit: Screen mode uses an integer scale that fits; the whole remote uses its zoom
    const fitScale = this.avail > 0 ? Math.max(1, Math.floor(this.avail / vb.w)) : 1;
    const z = screenMode ? Math.min(st.scale, fitScale) : st.zoom === '2' ? 2 : st.zoom === '1' ? 1 : 0;
    const dims = z ? { width: vb.w * z, height: vb.h * z } : { width: undefined, height: undefined };
    this.toggleAttribute('fit', !z);
    const frame = this.assigner.phase.name !== 'idle' && this.canTarget('screen');
    const frameOver = (this.assigner.cursor ?? (this.assigner.phase.name === 'dragging' ? this.assigner.phase.over : null))?.kind;
    void frameOver;
    const stripKeys = KEYS.filter((k) => k.group === 'strip');
    const hardKeys = KEYS.filter((k) => k.group !== 'strip');
    return html`<svg class="remote" viewBox="${vb.x} ${vb.y} ${vb.w} ${vb.h}" width=${dims.width ?? nothing} height=${dims.height ?? nothing} role="group" aria-label="Harmony Elite remote, ${st.layerName(st.layer)} layer" preserveAspectRatio="xMidYMin meet"
      @pointermove=${this.onMove} @pointerup=${this.onUp} @pointercancel=${this.onUp}>
      <defs>
        <linearGradient id="bodyg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2b3137"></stop><stop offset="0.4" stop-color="#1b2024"></stop><stop offset="1" stop-color="#15191d"></stop></linearGradient>
        <linearGradient id="keyg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3c444b"></stop><stop offset="1" stop-color="#252b30"></stop></linearGradient>
        <linearGradient id="keyh" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#48515a"></stop><stop offset="1" stop-color="#2d343a"></stop></linearGradient>
      </defs>
      ${st.xray && !screenMode ? this.xray(rows) : nothing}
      <rect class="body" x=${BODY.x} y=${BODY.y} width=${BODY.w} height=${BODY.h} rx=${BODY.r}></rect>
      <rect class="rim" x=${BODY.x + 3} y=${BODY.y + 3} width=${BODY.w - 6} height=${BODY.h - 6} rx=${BODY.r - 3}></rect>
      <rect class="glass" x=${GLASS.x} y=${GLASS.y} width=${GLASS.w} height=${GLASS.h} rx=${GLASS.r}></rect>
      <svg class="scr" x=${SCREEN.x} y=${SCREEN.y} width=${SCREEN.w} height=${SCREEN.h} viewBox="0 0 240 320" @pointerdown=${(e: PointerEvent) => this.onScreenDown(e)}>
        ${v2 ? pageGroup(v2, { activeAct: activityIndex(this.store.config, st.layer) }) : svg`<rect width="240" height="320" fill="#101820"></rect><text x="120" y="164" font-size="13" fill="#687888" text-anchor="middle">${this.store.pages.length || !this.store.loaded ? 'No preview for this page' : 'Add a page to see the screen'}</text>`}
        ${this.screenOverlay()}
      </svg>
      <rect class=${frame ? 'screen-frame tgt' : 'screen-frame'} x=${SCREEN.x - 2} y=${SCREEN.y - 2} width=${SCREEN.w + 4} height=${SCREEN.h + 4} rx="4"></rect>
      ${stripKeys.map((g) => this.keyEl(g, byKey.get(g.name), errKeys))}
      ${hardKeys.map((g) => this.keyEl(g, byKey.get(g.name), errKeys))}
    </svg>`;
  }
}
