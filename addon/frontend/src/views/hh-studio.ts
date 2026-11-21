import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { Assigner, moveCursor } from '../lib/assigner';
import { icon } from '../lib/icons';
import { keyByName, keyTitle } from '../lib/remote-layout';
import { parseHash } from '../lib/router';
import { setStyle } from '../lib/set-style';
import { shared } from '../lib/styles';
import { StudioState, type StudioHost } from '../lib/studio-state';
import {
  DEFAULT_LAYER, pageCap, addPage, assign, deletePage, describeSource, duplicatePage, errorTarget, firstFree, isRefusal, moveBox, movePage, pageIndexForLayer, placeWidget, removeWidget, resizeBox, templatePages, MAX_PAGES,
  type Source, type Target,
} from '../lib/studio-model';
import { StoreElement, confirmAction } from '../components/base';
import type { HhRemote } from '../components/hh-remote';
import { tid } from '../components/hh-remote';
import type { PageOp } from '../components/hh-page-strip';
import '../components/hh-remote';
import '../components/hh-palette';
import '../components/hh-inspector';
import '../components/hh-page-strip';
import '../components/hh-action-picker';

interface Press {
  source: Source;
  x: number;
  y: number;
  id: number;
  touch: boolean;
  started: boolean;
  timer: ReturnType<typeof setTimeout> | null;
}

const LAYER_PILLS = 5;
/** The remote studio: palette | remote canvas with layers, X-ray and screen editing | inspector. */
@customElement('hh-studio')
export class HhStudio extends StoreElement implements StudioHost {
  static override styles = [
    shared,
    css`
      :host { display: block; --pw: 292px; --iw: 300px; }
      @media (min-width: 1280px) { :host { --pw: 320px; --iw: 300px; } }
      .studio { display: grid; grid-template-columns: var(--pw) minmax(0, 1fr) var(--iw); height: max(560px, calc(100dvh - 108px)); border: 1px solid var(--line); border-radius: var(--r2); overflow: hidden; background: var(--sf); position: relative; }
      .pal, .insp { min-height: 0; min-width: 0; overflow: hidden; display: flex; flex-direction: column; background: var(--sf); }
      .pal { border-right: 1px solid var(--line); }
      .insp { border-left: 1px solid var(--line); }
      .pal > *, .insp > * { flex: 1; min-height: 0; }
      .mid { position: relative; display: flex; flex-direction: column; min-width: 0; min-height: 0; background: var(--cv); }
      .tb { display: flex; flex-wrap: nowrap; align-items: center; gap: 6px 12px; padding: 4px 12px; background: var(--sf); border-bottom: 1px solid var(--line); z-index: 2; }
      .lw { display: flex; align-items: center; gap: 4px; flex: 1 1 0; min-width: 0; }
      .layers { display: flex; gap: 4px; overflow: hidden; flex: 0 1 auto; min-width: 0; padding: 2px; }
      .lw select { flex: none; height: 28px; max-width: 120px; border-radius: 16px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 600; font-size: 12px; padding: 0 6px; }
      .layers button { flex: 0 1 auto; min-width: 0; max-width: 140px; overflow: hidden; text-overflow: ellipsis; height: 28px; padding: 0 14px; border-radius: 16px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 600; font-size: 13px; cursor: pointer; white-space: nowrap; }
      .layers button:hover { background: var(--sf2); }
      .layers button[aria-selected="true"] { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
      .grp { display: inline-flex; gap: 2px; padding: 2px; background: var(--sf2); border-radius: var(--r1); }
      .grp button { height: 28px; min-width: 34px; padding: 0 9px; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); font-weight: 600; font-size: 12px; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; gap: 4px; }
      .grp button[aria-pressed="true"], .grp button[aria-selected="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.14); }
      .grp button:disabled { opacity: 0.4; cursor: default; }
      .grp .ic { width: 16px; height: 16px; }
      @media (min-width: 1101px) { .tb { flex-wrap: wrap; } .lw { flex: 1 1 100%; } }
      @media (max-width: 720px) { .layers { overflow-x: auto; } .layers button { flex: none; } }
      @media (max-width: 1100px) { .layers button { padding: 0 9px; font-size: 12px; max-width: 96px; } }
      .cw { position: relative; flex: 1; min-height: 0; display: flex; flex-direction: column; }
      .tools { flex: none; display: flex; align-items: center; gap: 6px; }
      .tools .pager { background: var(--sf2); }
      .canvas { flex: 1; min-height: 0; overflow: auto; overscroll-behavior: contain; display: flex; flex-direction: column; background-color: var(--cv); background-image: radial-gradient(var(--dot) 1px, transparent 1px); background-size: 18px 18px; padding: 4px 16px; position: relative; scroll-behavior: auto; }
      hh-remote { flex: 1 1 auto; min-height: 0; }
      .banner2 { position: sticky; top: 0; z-index: 3; margin: -14px -16px 10px; flex: none; padding: 8px 14px; background: var(--acc); color: var(--onacc); font-weight: 600; display: flex; gap: 10px; align-items: center; }
      .banner2 .sp { flex: 1; }
      .banner2 button { height: 26px; padding: 0 10px; border-radius: 13px; border: 1px solid currentColor; background: transparent; color: inherit; cursor: pointer; font-weight: 600; }
      .ghost { position: fixed; z-index: 50; pointer-events: none; padding: 6px 12px; border-radius: 16px; background: var(--acc); color: var(--onacc); font-weight: 600; font-size: 13px; box-shadow: var(--lift); max-width: 460px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
      .ghost.bad { background: var(--bad); color: #fff; }
      .ghost small { font-weight: 400; opacity: 0.9; margin-left: 6px; }
      .pop { position: absolute; z-index: 8; width: 308px; max-height: 80%; overflow: auto; background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); box-shadow: var(--lift); padding: 12px; display: grid; gap: 10px; }
      .pop h3 { margin: 0; font-size: 14px; display: flex; align-items: center; gap: 8px; }
      .pop h3 .sp { flex: 1; }
      .pop .x { width: 28px; height: 28px; border: 0; border-radius: 14px; background: transparent; cursor: pointer; color: var(--tx2); display: grid; place-items: center; padding: 0; }
      .pop .x:hover { background: var(--sf2); }
      .keyerr { margin: 0 12px 8px; }
      .first { flex: none; max-width: 420px; margin: 0 auto 14px; padding: 14px 16px; background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); display: grid; gap: 6px; justify-items: start; box-shadow: var(--sh); }
      .first h2 { margin: 0; font-size: 16px; }
      .first p { margin: 0; color: var(--tx2); }
      .errbar { flex: none; margin: 0 0 10px; padding: 8px 12px; border-radius: var(--r2); background: var(--bads); border: 1px solid var(--bad); color: var(--tx); display: grid; gap: 4px; font-size: 13px; max-width: 520px; align-self: center; width: 100%; }
      .errbar div { display: flex; gap: 10px; align-items: center; }
      .errbar span { flex: 1; min-width: 0; overflow-wrap: anywhere; }
      .errbar button { height: 26px; padding: 0 10px; border-radius: 13px; border: 1px solid var(--bad); background: var(--sf); color: var(--tx); cursor: pointer; font-weight: 600; flex: none; }
      .pager { display: inline-flex; align-items: center; gap: 2px; }
      .sheetbtns { display: none; }
      .scrim { display: none; }
      @media (max-width: 1100px) { :host { --pw: 224px; --iw: 236px; } .studio { height: max(560px, calc(100dvh - 104px)); } .zoomg { display: none !important; } }
      /* X-ray is a reading mode: on narrower screens it takes the whole width so the labels stay legible */
      @media (max-width: 1280px) and (min-width: 861px) {
        .studio.xr { grid-template-columns: minmax(0, 1fr); }
        .studio.xr .pal, .studio.xr .insp { display: none; }
      }
      @media (max-width: 1100px) { .tools .grp button { min-width: 30px; padding: 0 7px; } }
      @media (max-width: 860px) {
        :host { --pw: 0px; --iw: 0px; }
        .studio { grid-template-columns: minmax(0, 1fr); height: max(520px, calc(100dvh - 132px)); }
        .pal, .insp { display: none; }
        :host([sheet="palette"]) .pal, :host([sheet="inspector"]) .insp { display: flex; position: fixed; left: 0; right: 0; bottom: 0; height: 62vh; z-index: 40; border: 0; border-top: 1px solid var(--line); border-radius: var(--r3) var(--r3) 0 0; box-shadow: var(--lift); }
        :host([sheet="palette"]) .insp, :host([sheet="inspector"]) .pal { display: none; }
        :host([sheet]) .scrim { display: block; position: fixed; inset: 0; z-index: 39; background: var(--scrim); }
        .sheetbtns { display: inline-flex; }
        .tb { flex-wrap: wrap; }
        .tools { flex: 1 1 100%; flex-wrap: wrap; }
        .pop { position: fixed; left: 8px; right: 8px; bottom: 8px; top: auto !important; width: auto; max-height: 62vh; z-index: 41; }
        .canvas { padding: 16px 8px 20px; }
        .banner2 { margin: -16px -8px 12px; }
      }
    `,
  ];

  state0 = new StudioState(this.store);
  get state(): StudioState {
    return this.state0;
  }
  assigner = new Assigner((s, t) => this.commit(s, t));
  @state() private pop: { x: number; y: number } | null = null;
  @state() private sheet: '' | 'palette' | 'inspector' = '';
  @state() private ghost: { x: number; y: number; text: string; bad: boolean } | null = null;
  private press: Press | null = null;
  private lastCfg: unknown = null;
  private inited = false;

  private rerender = () => this.requestUpdate();
  /** Pills the toolbar really has room for: starts at LAYER_PILLS and shrinks while a pill is squeezed, so the layout holds with any font. */
  private pillCap = LAYER_PILLS;
  private pillSig = '';
  private pillRo?: ResizeObserver;
  private resetPills = () => { this.pillCap = LAYER_PILLS; this.requestUpdate(); };
  protected override updated(): void {
    if (this.sheet) this.setAttribute('sheet', this.sheet);
    else this.removeAttribute('sheet');
    const sig = (this.store.config.activities ?? []).map((a) => a.name ?? a.id).join('\u0001');
    if (sig !== this.pillSig) { this.pillSig = sig; if (this.pillCap !== LAYER_PILLS) return this.resetPills(); }
    const row = this.renderRoot.querySelector<HTMLElement>('.layers');
    if (!row || this.pillCap <= 2) return;
    const btns = [...row.querySelectorAll('button')];
    if (row.scrollWidth > row.clientWidth + 1 || btns.some((b) => b.scrollWidth > b.clientWidth + 1)) {
      this.pillCap = Math.max(2, Math.min(this.pillCap, btns.length) - 1);
      this.requestUpdate();
    }
  }
  override connectedCallback(): void {
    super.connectedCallback();
    this.pillRo = new ResizeObserver(this.resetPills);
    this.pillRo.observe(this);
    void document.fonts?.ready.then(this.resetPills);
    this.state0.addEventListener('change', this.rerender);
    this.assigner.addEventListener('change', this.rerender);
    this.store.addEventListener('change', this.onStore);
    window.addEventListener('keydown', this.onKey);
    this.addEventListener('hh-chip-down', ((e: CustomEvent) => this.chipDown(e.detail.source, e.detail.ev)) as EventListener);
    this.addEventListener('hh-chip-pick', ((e: CustomEvent) => this.keyboardPick(e.detail.source)) as EventListener);
    this.addEventListener('hh-add-page', ((e: CustomEvent) => void this.pageOp({ op: 'add', template: e.detail })) as EventListener);
    this.addEventListener('hh-page-op', ((e: CustomEvent<PageOp>) => void this.pageOp(e.detail)) as unknown as EventListener);
    this.addEventListener('hh-inspector-pick', ((e: CustomEvent<Source>) => this.pickForSelection(e.detail)) as EventListener);
    void this.state0.loadCapabilities();
    this.onStore();
  }
  override disconnectedCallback(): void {
    this.state0.removeEventListener('change', this.rerender);
    this.assigner.removeEventListener('change', this.rerender);
    this.store.removeEventListener('change', this.onStore);
    window.removeEventListener('keydown', this.onKey);
    this.pillRo?.disconnect();
    this.endPress();
    super.disconnectedCallback();
  }
  private onStore = () => {
    const s = this.store;
    if (!s.loaded) return;
    if (!this.inited) {
      this.inited = true;
      this.state0.page = pageIndexForLayer(s.config, this.state0.layer);
      // deep link from Activities: #/remote/<layer>/<key>
      const [layer, key] = parseHash(location.hash).parts;
      if (layer && (layer === DEFAULT_LAYER || (s.config.activities ?? []).some((a) => a.id === layer))) {
        this.state0.setLayer(layer);
        if (key && keyByName.has(key)) this.state0.sel = { kind: 'key', name: key };
        const w = /^w(\d+)\.(\d+)$/.exec(key ?? '');
        if (w) {
          this.state0.page = Number(w[1]);
          this.state0.sel = { kind: 'widget', page: Number(w[1]), index: Number(w[2]) };
        }
      }
    }
    if (s.config !== this.lastCfg) {
      this.lastCfg = s.config;
      this.state0.scheduleKeymap();
      const n = s.config.pages?.length ?? 0;
      if (this.state0.page >= n) this.state0.page = Math.max(0, n - 1);
      if (this.state0.layer !== DEFAULT_LAYER && !(s.config.activities ?? []).some((a) => a.id === this.state0.layer)) this.state0.layer = DEFAULT_LAYER;
    }
  };
  private get remote(): HhRemote | null {
    return this.renderRoot.querySelector('hh-remote');
  }
  private get st() {
    return this.state0;
  }

  // ------------------------------------------------------------ the one commit path
  private commit(source: Source, target: Target): void {
    const r = assign(this.store.config, this.st.layer, source, target, this.st.haWritable);
    if (isRefusal(r)) {
      this.store.toast(r.refused, 'error');
      this.st.say(r.refused);
      return;
    }
    this.store.replace(r.config);
    if (r.key) this.st.sel = { kind: 'key', name: r.key };
    if (r.widget) {
      if (r.widget.page !== this.st.page) this.st.setPage(r.widget.page, false);
      this.st.sel = { kind: 'widget', page: r.widget.page, index: r.widget.index };
    }
    this.sheet = '';
    this.st.say(r.message);
    this.store.toast(r.message, 'info', true);
    this.pop = null;
    if (source.kind === 'widget' && r.widget) queueMicrotask(() => this.openPopover());
  }

  /** Apply a source to whatever is selected (inspector / popover pickers). */
  private pickForSelection(source: Source): void {
    const sel = this.st.sel;
    if (sel?.kind === 'key') this.commit(source, { kind: 'key', name: sel.name });
    else if (sel?.kind === 'widget') this.commit(source, { kind: 'widget', page: sel.page, index: sel.index });
  }

  // ------------------------------------------------------------ StudioHost
  activateKey(name: string): void {
    if (this.assigner.phase.name === 'picked') {
      this.assigner.choose({ kind: 'key', name });
      return;
    }
    if (this.st.sel?.kind === 'key' && this.st.sel.name === name) {
      this.deselect();
      return;
    }
    this.st.set({ sel: { kind: 'key', name } });
    this.st.say(`${keyTitle(name)} selected`);
    void this.updateComplete.then(() => this.openPopover());
  }
  deselect(): void {
    this.pop = null;
    if (this.st.sel && this.st.sel.kind !== 'page') this.st.set({ sel: null });
    this.st.say('Selection cleared');
  }
  activateWidget(page: number, index: number): void {
    if (this.assigner.phase.name === 'picked') {
      this.assigner.choose({ kind: 'widget', page, index });
      return;
    }
    if (page !== this.st.page) this.st.setPage(page, false);
    this.pop = null;
    this.st.set({ sel: { kind: 'widget', page, index } });
  }
  activateCell(page: number, x: number, y: number): void {
    if (this.assigner.phase.name === 'picked') {
      this.assigner.choose({ kind: 'cell', page, x, y });
      return;
    }
    this.pop = null;
    this.st.set({ sel: this.st.sel?.kind === 'page' ? this.st.sel : null });
  }
  setBox(page: number, index: number, box: { x: number; y: number; w: number; h: number }): void {
    this.store.replace(placeWidget(this.store.config, page, index, box));
    this.st.say(`Widget now at column ${box.x + 1}, row ${box.y + 1}, ${box.w} by ${box.h}`);
  }
  nudge(page: number, index: number, dx: number, dy: number, resize: boolean): void {
    const p = this.store.config.pages?.[page];
    if (!p) return;
    const b = resize ? resizeBox(p, index, dx, dy) : moveBox(p, index, dx, dy);
    if (!b) {
      this.st.say(resize ? 'Cannot resize further' : 'That spot is taken or off the grid');
      return;
    }
    this.store.replace(placeWidget(this.store.config, page, index, b), `nudge${page}.${index}`);
    this.st.say(`${resize ? 'Resized to' : 'Moved to column'} ${resize ? `${b.w} by ${b.h}` : `${b.x + 1}, row ${b.y + 1}`}`);
  }
  removeWidget(page: number, index: number): void {
    const w = this.store.config.pages?.[page]?.widgets[index];
    this.store.replace(removeWidget(this.store.config, page, index));
    this.st.set({ sel: null });
    this.st.say(`Deleted ${w?.type ?? 'widget'} ${w?.label ?? ''}`);
    this.store.toast(`Deleted ${w?.type ?? 'widget'} ${w?.label ?? ''}`.trim(), 'info', true);
  }
  addWidgetFree(): void {
    const page = this.store.config.pages?.[this.st.page];
    if (!page) return;
    const at = page.list ? { x: 0, y: 0 } : firstFree(page);
    if (!at) {
      this.st.say('This page is full');
      return;
    }
    this.commit({ kind: 'widget', type: 'button' }, { kind: 'cell', page: this.st.page, x: at.x, y: at.y });
  }
  openPopover(): void {
    const sel = this.st.sel;
    const wrap = this.renderRoot.querySelector<HTMLElement>('.cw');
    const root = this.remote?.shadowRoot;
    if (!wrap || !root || !sel || sel.kind === 'page') return;
    const el = root.querySelector<SVGGraphicsElement>(sel.kind === 'key' ? `[data-key="${sel.name}"]` : `[data-widget="${sel.index}"]`);
    if (!el) return;
    const a = el.getBoundingClientRect();
    const c = wrap.getBoundingClientRect();
    let x = a.right - c.left + 14;
    if (x + 308 > c.width - 8) x = a.left - c.left - 322;
    if (x < 8) x = Math.max(8, (c.width - 308) / 2);
    const y = Math.max(8, Math.min(a.top - c.top - 30, c.height - 340));
    this.pop = { x, y };
  }

  // ------------------------------------------------------------ pages
  private async pageOp(o: PageOp): Promise<void> {
    const cfg = this.store.config;
    const idx = o.index ?? this.st.page;
    if ((o.op === 'add' || o.op === 'dup') && pageCap(cfg)) {
      this.store.toast(pageCap(cfg) ?? '', 'error');
      return;
    }
    if (o.op === 'add') {
      if (o.template === 'lights' || o.template === 'activity') void this.st.loadHa();
      const made = templatePages(cfg, o.template ?? 'blank', { entities: this.st.entities ?? [], activity: this.st.layer === DEFAULT_LAYER ? undefined : this.st.layer });
      const room = Math.max(1, MAX_PAGES - (cfg.pages?.length ?? 0));
      const at = (cfg.pages?.length ?? 0) === 0 ? 0 : this.st.page + 1;
      const kept = made.slice(0, room);
      let next = cfg;
      kept.forEach((pg, i) => (next = addPage(next, pg, at + i)));
      this.store.replace(next);
      this.st.setPage(at);
      const what = kept.length > 1 ? `${kept.length} pages` : `page ${kept[0]?.title ?? kept[0]?.id}`;
      this.st.say(`Added ${what}`);
      this.store.toast(`Added ${what}`, 'info', true);
    } else if (o.op === 'dup') {
      this.store.replace(duplicatePage(cfg, idx));
      this.st.setPage(idx + 1);
      this.st.say('Duplicated page');
    } else if (o.op === 'del') {
      const p = cfg.pages?.[idx];
      if (!p) return;
      const ok = await confirmAction(this, { title: 'Delete Page?', body: `"${p.title || p.id}" and its ${p.widgets.length} widgets will be removed. You can undo this.`, ok: 'Delete Page', danger: true });
      if (!ok) return;
      this.store.replace(deletePage(this.store.config, idx));
      this.st.setPage(Math.max(0, Math.min(this.st.page, (this.store.config.pages?.length ?? 1) - 1)), false);
      this.st.say(`Deleted page ${p.title || p.id}`);
      this.store.toast(`Deleted page ${p.title || p.id}`, 'info', true);
    } else if (o.op === 'move' && o.to !== undefined) {
      this.store.replace(movePage(cfg, idx, o.to));
      this.st.setPage(o.to);
    }
  }

  // ------------------------------------------------------------ drag, click-pick, keyboard
  private chipDown(source: Source, ev: PointerEvent): void {
    this.endPress();
    const press: Press = { source, x: ev.clientX, y: ev.clientY, id: ev.pointerId, touch: ev.pointerType === 'touch', started: false, timer: null };
    this.press = press;
    this.pressOrigin = { x: ev.clientX, y: ev.clientY };
    if (press.touch) press.timer = setTimeout(() => this.beginDrag(), 350);
    window.addEventListener('pointermove', this.onPointerMove);
    window.addEventListener('pointerup', this.onPointerUp);
    window.addEventListener('pointercancel', this.onPointerCancel);
    window.addEventListener('touchmove', this.onTouchMove, { passive: false });
  }
  private beginDrag(): void {
    const p = this.press;
    if (!p || p.started) return;
    p.started = true;
    this.assigner.startDrag(p.source, p.x, p.y);
    this.sheet = '';
    this.st.say(`Dragging ${describeSource(this.store.config, p.source)}. Release on a key or the screen.`);
  }
  private onTouchMove = (e: TouchEvent) => {
    if (this.press?.started) e.preventDefault();
  };
  private onPointerMove = (e: PointerEvent) => {
    const p = this.press;
    if (!p || e.pointerId !== p.id) return;
    p.x = e.clientX;
    p.y = e.clientY;
    if (!p.started) {
      const far = Math.hypot(e.clientX - this.pressOrigin.x, e.clientY - this.pressOrigin.y) > 6;
      if (!far) return;
      if (p.touch) return this.endPress(); // a touch that moves first is a scroll, not a drag
      this.beginDrag();
    }
    const target = this.remote?.targetAt(e.clientX, e.clientY) ?? null;
    this.assigner.moveDrag(e.clientX, e.clientY, target);
    this.updateGhost(e.clientX, e.clientY, target);
    this.autoScroll(e.clientY);
  };
  private pressOrigin = { x: 0, y: 0 };
  private onPointerUp = (e: PointerEvent) => {
    const p = this.press;
    if (!p || e.pointerId !== p.id) return;
    const started = p.started;
    const source = p.source;
    this.endPress();
    if (started) {
      const target = this.remote?.targetAt(e.clientX, e.clientY) ?? null;
      const ok = target && !isRefusal(assign(this.store.config, this.st.layer, source, target, this.st.haWritable) as never);
      this.ghost = null;
      if (!this.assigner.drop(ok ? target : null) && target) {
        // refused: say why
        const r = assign(this.store.config, this.st.layer, source, target, this.st.haWritable);
        if (isRefusal(r)) {
          this.st.say(r.refused);
          this.store.toast(r.refused, 'error');
        }
      } else if (!target) this.st.say('Drop cancelled: nothing there');
    } else {
      // a plain click or tap: pick up (or put down) the source
      this.assigner.pick(source);
      this.sheet = '';
      if (this.assigner.phase.name === 'picked') this.st.say(`Picked up ${describeSource(this.store.config, source)}. Click a key or the screen, or use the arrow keys and Enter.`);
    }
  };
  private onPointerCancel = (e: PointerEvent) => {
    if (this.press && e.pointerId === this.press.id) {
      this.endPress();
      this.assigner.cancel();
      this.ghost = null;
    }
  };
  private endPress(): void {
    if (this.press?.timer) clearTimeout(this.press.timer);
    this.press = null;
    window.removeEventListener('pointermove', this.onPointerMove);
    window.removeEventListener('pointerup', this.onPointerUp);
    window.removeEventListener('pointercancel', this.onPointerCancel);
    window.removeEventListener('touchmove', this.onTouchMove);
  }
  private updateGhost(x: number, y: number, target: Target | null): void {
    const p = this.assigner.phase;
    if (p.name !== 'dragging') return;
    const label = describeSource(this.store.config, p.source);
    let bad = false;
    let tail = '';
    if (target) {
      const r = assign(this.store.config, this.st.layer, p.source, target, this.st.haWritable);
      bad = isRefusal(r);
      tail = bad ? (r as { refused: string }).refused : this.targetName(target);
    }
    this.ghost = { x: x + 14, y: y + 12, text: label, bad };
    this.ghostTail = tail;
  }
  private ghostTail = '';
  private autoScroll(y: number): void {
    const c = this.renderRoot.querySelector<HTMLElement>('.canvas');
    if (!c) return;
    const r = c.getBoundingClientRect();
    if (y < r.top + 36) c.scrollTop -= 14;
    else if (y > r.bottom - 36) c.scrollTop += 14;
  }
  private targetName(t: Target): string {
    if (t.kind === 'key') return `${keyTitle(t.name)}${this.st.layer === DEFAULT_LAYER ? '' : ' (overrides Default)'}`;
    const p = this.store.config.pages?.[t.page];
    if (t.kind === 'cell') return `${p?.title || p?.id || 'page'}, column ${t.x + 1} row ${t.y + 1}`;
    return `${p?.widgets[t.index]?.type ?? 'widget'} ${p?.widgets[t.index]?.label ?? ''}`.trim();
  }

  private keyboardPick(source: Source): void {
    this.assigner.pick(source);
    this.sheet = '';
    if (this.assigner.phase.name === 'picked') {
      const first = this.remote?.spots().find((s) => s.target.kind === 'key' && s.target.name === (this.st.sel?.kind === 'key' ? this.st.sel.name : 'select'));
      if (first) this.assigner.setCursor(first.target);
      this.st.say(`Picked up ${describeSource(this.store.config, source)}. Arrow keys choose a target, Enter drops it, Escape cancels.`);
    } else this.st.say('Put down');
  }
  private onKey = (e: KeyboardEvent) => {
    const inField = (e.composedPath()[0] as HTMLElement | undefined)?.matches?.('input, textarea, select, [contenteditable]');
    if (e.key === 'Escape') {
      if (this.assigner.active) {
        this.assigner.cancel();
        this.ghost = null;
        this.endPress();
        this.st.say('Cancelled');
      } else if (this.pop) this.pop = null;
      else if (this.sheet) this.sheet = '';
      else if (!inField && this.st.sel) this.st.set({ sel: null });
      return;
    }
    if ((e.ctrlKey || e.metaKey) && !inField && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      if (e.shiftKey ? this.store.redo() : this.store.undo()) this.st.say(e.shiftKey ? 'Redone' : 'Undone');
      return;
    }
    if ((e.ctrlKey || e.metaKey) && !inField && e.key.toLowerCase() === 'y') {
      e.preventDefault();
      if (this.store.redo()) this.st.say('Redone');
      return;
    }
    if (this.assigner.phase.name === 'picked' && !inField) {
      const dirs: Record<string, 'left' | 'right' | 'up' | 'down'> = { ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down' };
      const d = dirs[e.key];
      if (d && this.remote) {
        e.preventDefault();
        const cur = this.assigner.cursor;
        const next = moveCursor(this.remote.spots(), cur ? tid(cur) : null, d);
        if (next) {
          this.assigner.setCursor(next.target);
          this.st.say(this.targetName(next.target));
          this.scrollCursorIntoView();
        }
      } else if (e.key === 'Enter' && this.assigner.cursor) {
        e.preventDefault();
        const cur = this.assigner.cursor;
        this.assigner.choose(cur);
      }
    }
  };
  private scrollCursorIntoView(): void {
    const c = this.assigner.cursor;
    if (!c) return;
    const el = this.remote?.shadowRoot?.querySelector(c.kind === 'key' ? `[data-key="${c.name}"]` : c.kind === 'widget' ? `[data-widget="${c.index}"]` : 'svg.scr');
    (el as HTMLElement | null)?.scrollIntoView?.({ block: 'nearest', inline: 'nearest' });
  }

  // ------------------------------------------------------------ render
  private layers() {
    const acts = this.store.config.activities ?? [];
    const items: [string, string][] = [[DEFAULT_LAYER, 'Default'], ...acts.map((a) => [a.id, a.name ?? a.id] as [string, string])];
    const cap = this.pillCap;
    // up to LAYER_PILLS pills; beyond that the rest sit in a menu (the selected layer always shows as a pill)
    let shown = items;
    let rest: [string, string][] = [];
    if (items.length > cap) {
      const head = items.slice(0, cap - 1);
      const cur = items.find(([id]) => id === this.st.layer);
      shown = cur && !head.includes(cur) ? [...items.slice(0, cap - 2), cur] : head;
      rest = items.filter((it) => !shown.includes(it));
    }
    return html`<div class="lw"><div class="layers" role="tablist" aria-label="Layer" @keydown=${(e: KeyboardEvent) => this.layerKey(e, items)}>
      ${shown.map(([id, name]) => html`<button role="tab" id=${`layer-${id}`} title=${name} tabindex=${this.st.layer === id ? 0 : -1} aria-selected=${this.st.layer === id} @click=${() => this.st.setLayer(id)}>${name}</button>`)}
    </div>${rest.length ? html`<select aria-label="More layers" @change=${(e: Event) => { const v = (e.target as HTMLSelectElement).value; if (v) this.st.setLayer(v); (e.target as HTMLSelectElement).value = ''; }}>
      <option value="">More (${rest.length})</option>${rest.map(([id, name]) => html`<option value=${id}>${name}</option>`)}
    </select>` : nothing}</div>`;
  }
  private layerKey(e: KeyboardEvent, items: [string, string][]) {
    const i = items.findIndex(([id]) => id === this.st.layer);
    const to = e.key === 'ArrowRight' ? i + 1 : e.key === 'ArrowLeft' ? i - 1 : e.key === 'Home' ? 0 : e.key === 'End' ? items.length - 1 : -1;
    const t = items[to];
    if (!t) return;
    e.preventDefault();
    this.st.setLayer(t[0]);
    void this.updateComplete.then(() => this.renderRoot.querySelector<HTMLElement>(`#layer-${t[0]}`)?.focus());
  }

  protected override render() {
    const st = this.st;
    const s = this.store;
    const picked = this.assigner.phase.name === 'picked' ? this.assigner.phase.source : null;
    const sel = st.sel;
    const popKind = sel?.kind;
    const screenMode = st.mode === 'screen';
    return html`
      <div class=${classMap({ studio: true, xr: st.xray && !screenMode })}>
        <aside class="pal" aria-label="Palette"><hh-palette .host=${this} .assigner=${this.assigner}></hh-palette></aside>
        <section class="mid" aria-label="Remote canvas">
          <div class="tb">
            ${this.layers()}
            <div class="tools">
            <div class="grp" role="group" aria-label="View">
              <button aria-pressed=${!screenMode} @click=${() => st.set({ mode: 'remote' })}>Remote</button>
              <button aria-pressed=${screenMode} @click=${() => st.set({ mode: 'screen' })}>Screen</button>
            </div>
            ${screenMode
              ? html`<div class="grp" role="group" aria-label="Screen scale">${[1, 2, 3].map((n) => html`<button aria-pressed=${st.scale === n} @click=${() => st.set({ scale: n })}>${n}×</button>`)}</div>`
              : html`<div class="grp zoomg" role="group" aria-label="Zoom">${(['fit', '1', '2'] as const).map((z) => html`<button aria-pressed=${st.zoom === z} @click=${() => st.set({ zoom: z })}>${z === 'fit' ? 'Fit' : `${z}×`}</button>`)}</div>`}
            <div class="grp">
              <button aria-pressed=${st.xray} ?disabled=${screenMode} title="Label every key with its action" @click=${() => st.set({ xray: !st.xray })}>X-ray</button>
            </div>
            <div class="grp" role="group" aria-label="History">
              <button aria-label="Undo" title="Undo (Ctrl+Z)" ?disabled=${!s.canUndo} @click=${() => s.undo() && st.say('Undone')}>${icon('undo')}</button>
              <button aria-label="Redo" title="Redo (Ctrl+Shift+Z)" ?disabled=${!s.canRedo} @click=${() => s.redo() && st.say('Redone')}>${icon('redo')}</button>
            </div>
            ${this.pager()}
            <div class="grp sheetbtns" role="group" aria-label="Panels">
              <button @click=${() => (this.sheet = this.sheet === 'palette' ? '' : 'palette')}>Palette</button>
              <button @click=${() => (this.sheet = this.sheet === 'inspector' ? '' : 'inspector')}>Inspector</button>
            </div>
            </div>
          </div>
          <div class="cw">
          <div class="canvas" @click=${(e: MouseEvent) => { const t = e.composedPath()[0] as HTMLElement; if ((t === e.currentTarget || t.localName === 'hh-remote') && this.assigner.phase.name === 'idle') this.deselect(); }} @pointerdown=${(e: PointerEvent) => { if ((e.target as HTMLElement).closest('hh-remote') === null && this.pop) this.pop = null; }} @scroll=${() => (this.pop = null)}>
            ${picked
              ? html`<div class="banner2" role="status"><span>Click a key or the screen for ${describeSource(s.config, picked)}</span><span class="sp"></span><button @click=${() => this.assigner.cancel()}>Cancel</button></div>`
              : nothing}
            ${this.errorBar()}
            ${s.isFirstRun
              ? html`<div class="first" role="status">
                  <h2>Set Up Your First Hub Config</h2>
                  <p>Nothing has been applied yet. Import a YAML file you already have, then drag its commands onto the remote.</p>
                  <button class="btn pri" @click=${() => this.dispatchEvent(new Event('hh-import', { bubbles: true, composed: true }))}>Import YAML File</button>
                </div>`
              : nothing}
            <hh-remote .host=${this} .assigner=${this.assigner}></hh-remote>
          </div>
          ${this.pop && popKind && popKind !== 'page' ? this.renderPop() : nothing}
          </div>
          <hh-page-strip .host=${this}></hh-page-strip>
        </section>
        <aside class="insp" aria-label="Inspector"><hh-inspector .host=${this} .assigner=${this.assigner}></hh-inspector></aside>
        <div class="scrim" @click=${() => (this.sheet = '')}></div>
      </div>
      ${this.ghost ? html`<div class=${classMap({ ghost: true, bad: this.ghost.bad })} style=${setStyle({ left: `${this.ghost.x}px`, top: `${this.ghost.y}px` })}>${this.ghost.text}${this.ghostTail ? html`<small>${this.ghost.bad ? '' : '→ '}${this.ghostTail}</small>` : nothing}</div>` : nothing}
      <div class="vh" role="status" aria-live="polite" aria-atomic="true">${st.announcement}</div>`;
  }

  private showError(path: (string | number)[]): void {
    const t = errorTarget(path);
    if (!t) return;
    this.pop = null;
    if (t.kind === 'key') {
      const idx = t.layer.startsWith('#') ? Number(t.layer.slice(1)) : -1;
      const layer = idx >= 0 ? ((this.store.config.activities ?? [])[idx]?.id ?? DEFAULT_LAYER) : DEFAULT_LAYER;
      this.st.setLayer(layer);
      this.st.set({ sel: { kind: 'key', name: t.key } });
    } else if (t.kind === 'widget') {
      this.st.setPage(t.page, false);
      this.st.set({ sel: { kind: 'widget', page: t.page, index: t.index } });
    } else this.st.setPage(t.page);
    this.st.say('Showing the problem');
  }
  private errorBar() {
    const errs = this.store.errors;
    if (!errs.length) return nothing;
    const shown = errs.slice(0, 2);
    return html`<div class="errbar" role="alert">${shown.map((e) => html`<div><span>${e.msg}</span>${errorTarget(e.path) ? html`<button @click=${() => this.showError(e.path)}>Show</button>` : nothing}</div>`)}
      ${errs.length > 2 ? html`<span class="muted">and ${errs.length - 2} more in the draft bar</span>` : nothing}</div>`;
  }
  private pager() {
    const pp = this.st.previewOf(this.st.page);
    const n = pp?.wire_pages.length ?? 0;
    if (n < 2) return nothing;
    const i = Math.min(this.st.wire, n - 1);
    return html`<div class="grp pager" role="group" aria-label="Screens of this list page">
      <button aria-label="Previous screen" ?disabled=${i === 0} @click=${() => this.st.set({ wire: i - 1 })}>‹</button><span class="muted" aria-live="polite">${i + 1}/${n}</span>
      <button aria-label="Next screen" ?disabled=${i >= n - 1} @click=${() => this.st.set({ wire: i + 1 })}>›</button></div>`;
  }

  private renderPop() {
    const sel = this.st.sel;
    const pop = this.pop;
    if (!pop || !sel || sel.kind === 'page') return nothing;
    const cfg = this.store.config;
    let title = '';
    let current = '';
    let value: unknown = null;
    let mode: 'key' | 'widget' = 'key';
    if (sel.kind === 'key') {
      title = keyTitle(sel.name);
      const layer = this.st.layer;
      value = (layer === DEFAULT_LAYER ? cfg.keys : (cfg.activities ?? []).find((a) => a.id === layer)?.keys)?.[sel.name] ?? null;
      const row = this.st.rows().find((r) => r.key === (sel as { name: string }).name);
      current = row?.action ? `${this.st.actionLabel(row.action)}${row.source === 'default' && layer !== DEFAULT_LAYER ? ' (from Default)' : ''}` : '';
    } else {
      const w = cfg.pages?.[sel.page]?.widgets[sel.index];
      title = `${w?.type ?? 'widget'} ${w?.label ?? ''}`.trim();
      value = w?.['action'] ?? null;
      mode = 'widget';
    }
    return html`<div class="pop" role="dialog" aria-label="Action for ${title}" style=${setStyle({ left: `${pop.x}px`, top: `${pop.y}px` })}>
      <h3>${title}<span class="sp"></span><button class="x" aria-label="Close" @click=${() => (this.pop = null)}>${icon('x')}</button></h3>
      <hh-action-picker .host=${this} .value=${value} .current=${current} mode=${mode} ?inherits=${this.st.layer !== DEFAULT_LAYER} @pick=${(e: CustomEvent<Source>) => this.pickForSelection({ ...e.detail })}></hh-action-picker>
    </div>`;
  }
}
