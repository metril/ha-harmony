import { LitElement, css, html, nothing, svg } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { pageGroup } from '../lib/page-renderer';
import { setStyle } from '../lib/set-style';
import { shared } from '../lib/styles';
import type { StudioHost } from '../lib/studio-state';
import { TEMPLATES, errorTarget, pageBytes, type TemplateId } from '../lib/studio-model';
import { icon } from '../lib/icons';

export interface PageOp {
  op: 'add' | 'dup' | 'del' | 'move';
  index?: number;
  template?: TemplateId;
  to?: number;
}

/** Thumbnails of every page under the remote, drawn by the same renderer as the screen. */
@customElement('hh-page-strip')
export class HhPageStrip extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; background: var(--sf); border-top: 1px solid var(--line); }
      .strip { display: flex; gap: 8px; align-items: stretch; padding: 6px 12px 6px; overflow-x: auto; }
      .items { display: flex; gap: 8px; align-items: stretch; }
      .pg { position: relative; display: grid; gap: 3px; width: 84px; flex: none; justify-items: center; padding: 6px 4px 4px; border-radius: var(--r2); border: 1.5px solid transparent; background: transparent; cursor: pointer; user-select: none; -webkit-user-select: none; touch-action: pan-x; }
      .pg:hover { background: var(--sf2); }
      .pg[aria-current="true"] { border-color: var(--acc); background: var(--accs); }
      .pg.drop::before { content: ""; position: absolute; left: -8px; top: 8px; bottom: 8px; width: 3px; border-radius: 2px; background: var(--acc); }
      .pg.dragging { opacity: 0.5; }
      .thumb { width: 54px; height: 72px; border-radius: 6px; overflow: hidden; background: #101820; box-shadow: 0 0 0 1px var(--line); display: block; }
      .name { font-size: 12px; font-weight: 600; max-width: 78px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--tx); }
      .sub { font-size: 11px; color: var(--tx2); display: flex; gap: 6px; align-items: center; }
      .tw { position: relative; display: block; }
      .tw.multi .thumb { box-shadow: 0 0 0 1px var(--line), 4px 4px 0 -1px var(--sf3), 4px 4px 0 0 var(--line), 8px 8px 0 -2px var(--sf3), 8px 8px 0 -1px var(--line); margin: 0 8px 8px 0; }
      .seq, .kd { position: absolute; font-size: 10px; line-height: 14px; font-weight: 700; padding: 0 4px; border-radius: 7px; background: var(--acc); color: var(--onacc); pointer-events: none; }
      .seq { right: 8px; bottom: 10px; }
      .tw:not(.multi) .seq { display: none; }
      .kd { left: 2px; top: 2px; background: var(--ir); color: var(--sf); }
      .bar { width: 54px; height: 4px; }
      .err { position: absolute; top: 4px; right: 6px; width: 10px; height: 10px; border-radius: 50%; background: var(--bad); box-shadow: 0 0 0 2px var(--sf); }
      .tools { display: none; position: absolute; top: 2px; left: 2px; gap: 2px; }
      .pg:hover .tools, .pg:focus-within .tools { display: flex; }
      .tools button { width: 24px; height: 24px; border-radius: 12px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); cursor: pointer; display: grid; place-items: center; padding: 0; }
      .tools button:hover { background: var(--sf3); color: var(--tx); }
      .tools .ic { width: 14px; height: 14px; }
      .add { width: 84px; flex: none; display: grid; place-items: center; align-content: center; gap: 4px; border: 1.5px dashed var(--line); border-radius: var(--r2); color: var(--tx2); background: transparent; cursor: pointer; font-size: 12px; font-weight: 600; min-height: 100px; }
      .add:hover { background: var(--sf2); color: var(--tx); }
      .add .ic { width: 22px; height: 22px; }
      .menu { position: absolute; z-index: 9; bottom: calc(100% + 4px); left: 14px; width: min(300px, calc(100vw - 32px)); background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); box-shadow: var(--sh); padding: 6px; display: grid; gap: 2px; }
      .menu button { display: grid; text-align: left; gap: 1px; padding: 8px 10px; border: 0; border-radius: var(--r1); background: transparent; color: var(--tx); cursor: pointer; font-weight: 600; }
      .menu button:hover { background: var(--sf2); }
      .menu small { font-weight: 400; color: var(--tx3); font-size: 12px; }
      :host { position: relative; }
      @media (max-width: 1100px), (max-height: 820px) {
        .strip { padding: 2px 10px; gap: 6px; }
        .pg { width: 64px; padding: 4px 2px 2px; gap: 2px; }
        .thumb { width: 38px; height: 50px; }
        .name { font-size: 11px; max-width: 60px; }
        .sub { display: none; }
        .bar { width: 38px; height: 3px; }
        .add { width: 56px; min-height: 0; }
      }
    `,
  ];
  @property({ attribute: false }) host!: StudioHost;
  @state() private menu = false;
  @state() private dragFrom = -1;
  @state() private dropAt = -1;
  private bound: EventTarget[] = [];
  private rerender = () => this.requestUpdate();
  protected override updated(): void {
    const now = this.host ? [this.host.state, this.host.state.store] : [];
    if (now.length === this.bound.length && now.every((t, i) => t === this.bound[i])) return;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = now;
    now.forEach((t) => t.addEventListener('change', this.rerender));
  }
  override disconnectedCallback(): void {
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = [];
    super.disconnectedCallback();
  }
  private op(o: PageOp) {
    this.dispatchEvent(new CustomEvent('hh-page-op', { detail: o, bubbles: true, composed: true }));
  }

  // ---- reorder by pointer
  private down(e: PointerEvent, i: number) {
    if (e.button !== 0 || (e.target as HTMLElement).closest('.tools')) return;
    const startX = e.clientX;
    let moving = false;
    const move = (m: PointerEvent) => {
      if (!moving && Math.abs(m.clientX - startX) < 8) return;
      moving = true;
      this.dragFrom = i;
      const items = [...this.renderRoot.querySelectorAll<HTMLElement>('.pg')];
      let at = items.findIndex((el) => m.clientX < el.getBoundingClientRect().left + el.offsetWidth / 2);
      if (at < 0) at = items.length;
      this.dropAt = at;
    };
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', up);
      const from = this.dragFrom;
      const to = this.dropAt > from ? this.dropAt - 1 : this.dropAt;
      this.dragFrom = this.dropAt = -1;
      if (moving && from >= 0 && to >= 0 && to !== from) this.op({ op: 'move', index: from, to });
      else if (!moving) this.host.state.setPage(i);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', up);
  }
  private key(e: KeyboardEvent, i: number, n: number) {
    if (e.altKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
      e.preventDefault();
      const to = i + (e.key === 'ArrowLeft' ? -1 : 1);
      if (to >= 0 && to < n) {
        this.op({ op: 'move', index: i, to });
        this.host.state.say(`Moved page to position ${to + 1}`);
        queueMicrotask(() => void this.updateComplete.then(() => this.renderRoot.querySelectorAll<HTMLElement>('.pg')[to]?.focus()));
      }
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      this.host.state.setPage(i);
    } else if (!e.altKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
      const t = this.renderRoot.querySelectorAll<HTMLElement>('.pg')[i + (e.key === 'ArrowLeft' ? -1 : 1)];
      if (t) {
        e.preventDefault();
        t.focus();
      }
    }
  }

  protected override render() {
    if (!this.host) return nothing;
    const st = this.host.state;
    const cfg = st.store.config;
    const pages = cfg.pages ?? [];
    return html`
      <div class="strip">
        <div class="items" role="list" aria-label="Pages (Alt+Left and Alt+Right reorder)">
        ${pages.map((p, i) => {
          const pp = st.previewOf(i);
          const v2 = pp?.wire_pages[0]?.page ?? pp?.page;
          const bytes = pp?.bytes ?? pageBytes(p);
          const limit = pp?.limit ?? 251;
          const ratio = bytes / limit;
          const bad = st.store.errors.some((e) => {
            const t = errorTarget(e.path);
            return t && (t.kind === 'page' || t.kind === 'widget') && t.page === i;
          }) || pp?.ok === false;
          const name = p.title || p.id;
          const screens = pp?.wire_pages.length ?? 1;
          return html`<div class=${classMap({ pg: true, drop: this.dropAt === i && this.dragFrom !== i && this.dragFrom + 1 !== i, dragging: this.dragFrom === i })} role="listitem" tabindex="0" aria-current=${st.page === i} aria-label="${name}, page ${i + 1} of ${pages.length}${p.kind ? `, ${p.kind} page` : ''}${screens > 1 ? `, ${screens} screens` : ''}, ${bytes} of ${limit} bytes${bad ? ', has an error' : ''}"
            @pointerdown=${(e: PointerEvent) => this.down(e, i)} @keydown=${(e: KeyboardEvent) => this.key(e, i, pages.length)}>
            <span class=${classMap({ tw: true, multi: screens > 1 })}>
              <svg class="thumb" viewBox="0 0 240 320" aria-hidden="true">${v2 ? pageGroup(v2, { noStatus: true }) : svg`<rect width="240" height="320" fill="#101820"></rect>`}</svg>
              ${p.kind ? html`<span class="kd" aria-hidden="true">${p.kind === 'home' ? 'Home' : 'Fav'}</span>` : nothing}
              <span class="seq" aria-hidden="true">${st.page === i ? `${Math.min(st.wire, screens - 1) + 1}/${screens}` : `×${screens}`}</span>
            </span>
            <span class="name">${name}</span>
            ${p.kind || screens > 1 ? html`<span class="sub">${p.kind ?? ''}${p.kind && screens > 1 ? ' · ' : ''}${screens > 1 ? `${screens} screens` : ''}</span>` : nothing}
            <div class="bar" role="img" aria-label="${bytes} of ${limit} bytes"><div class="bar"><i class=${classMap({ warn: ratio >= 0.85 && ratio < 1, full: ratio >= 1 })} style=${setStyle({ width: `${Math.min(100, ratio * 100)}%` })}></i></div></div>
            ${bad ? html`<span class="err" role="img" aria-label="Error"></span>` : nothing}
            <div class="tools">
              <button aria-label="Duplicate ${name}" title="Duplicate" @click=${() => this.op({ op: 'dup', index: i })}>${icon('plus')}</button>
              <button aria-label="Delete ${name}" title="Delete" @click=${() => this.op({ op: 'del', index: i })}>${icon('x')}</button>
            </div>
          </div>`;
        })}
        </div>
        <button class="add" aria-haspopup="menu" aria-expanded=${this.menu} @click=${() => (this.menu = !this.menu)}>${icon('plus')}Add page</button>
      </div>
      ${this.menu
        ? html`<div class="menu" role="menu" @keydown=${(e: KeyboardEvent) => e.key === 'Escape' && (this.menu = false)}>
            ${TEMPLATES.map((t) => html`<button role="menuitem" @click=${() => { this.menu = false; this.op({ op: 'add', template: t.id }); }}>${t.name}<small>${t.note}</small></button>`)}
          </div>`
        : nothing}`;
  }
}
