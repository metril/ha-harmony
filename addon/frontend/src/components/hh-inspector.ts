import { LitElement, css, html, nothing, svg } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import type { Assigner } from '../lib/assigner';
void import('./hh-icon-picker');
import { alignApplies } from '../lib/page-renderer';
import { setStyle } from '../lib/set-style';
import { stockIconByName } from '../lib/stock-icons';
import { fitCells, watchCells } from '../lib/fit-cells';
import { abbreviate, displayCommand, displayKey, displayName, gridStyle, FREE_CAP, dn } from '../lib/key-names';
import { KEYS, keyTitle } from '../lib/remote-layout';
import { shared } from '../lib/styles';
import { collectIssues, widgetIssues } from '../lib/issues';
import type { StudioHost } from '../lib/studio-state';
import { meter, orderedCells } from './parts';
import { confirmAction } from './base';
import type { AddPaletteDetail } from './hh-colour-field';
import { PALETTE_NAMES } from '../lib/themes';
import { PRESETS, activeName, activeTheme, colourBytes, widgetColors } from '../lib/theme-model';
import {
  DEFAULT_LAYER, LABEL_MAX, PAGE_BUDGET, describeAction, errorTarget, gridOf, isList, labelBytes, labelMeter, labelRoom, pageBytes, setPage, setWidget, trimToBytes, wireIconOnly,
  sourceAction, type Source,
} from '../lib/studio-model';
import type { Page, Widget } from '../lib/model';
import { textPx } from '../lib/text-fit';
import { tileDefaultTap } from '../lib/permissions';

const ROLES = ['default', 'primary', 'secondary', 'danger'] as const;
const SHAPES = ['card', 'row', 'pill', 'monogram'] as const;
/** Alignment of the icon + label group; centre is the default for every shape, so choosing it clears `align`. */
const ALIGNS = [
  { v: 'left', name: 'Align Left', d: 'M4 6h16M4 12h10M4 18h13' },
  { v: 'center', name: 'Align Center', d: 'M4 6h16M7 12h10M5.5 18h13' },
  { v: 'right', name: 'Align Right', d: 'M4 6h16M10 12h10M7 18h13' },
] as const;

/** Right pane: layer summary, hard key, widget or page settings. */
@customElement('hh-inspector')
export class HhInspector extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; background: var(--sf); height: 100%; overflow: auto; }
      .pad { padding: 14px 14px 24px; display: grid; gap: 14px; align-content: start; }
      h2 { margin: 0; font-size: 14px; font-weight: 650; display: flex; gap: 8px; align-items: center; }
      h3 { margin: 0 0 6px; font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; }
      .sec { display: grid; gap: 8px; }
      label { display: grid; gap: 4px; font-size: 12px; font-weight: 600; color: var(--tx2); }
      input[type="text"], input[type="number"], input[type="search"], select { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; width: 100%; min-width: 0; }
      .cnt { display: flex; justify-content: space-between; font-weight: 500; color: var(--tx3); }
      .cnt.warn { color: var(--ir); } .cnt.full { color: var(--bad); }
      .pxm { margin: -4px 0 8px; font-size: 12px; }
      .issue { margin: 2px 0; font-size: 12px; overflow-wrap: anywhere; padding: 6px 8px; border-radius: var(--r2); background: var(--sf2); border: 1px solid var(--line); }
      .issue.warning { border-left: 3px solid var(--ir); } .issue.error { border-left: 3px solid var(--bad); }
      .issue .chip { margin-top: 4px; border: 1px solid var(--line); background: var(--sf); color: var(--tx); border-radius: 12px; padding: 2px 10px; cursor: pointer; }
      .pxm .chips { display: flex; flex-wrap: wrap; gap: 4px; }
      .pxm .chip { border: 1px solid var(--line); background: var(--sf2); color: var(--tx); border-radius: 12px; padding: 2px 10px; cursor: pointer; }
      .seg { display: flex; flex-wrap: wrap; gap: 4px; }
      .seg button { height: 30px; padding: 0 10px; border-radius: 15px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); cursor: pointer; font-size: 12px; font-weight: 600; text-transform: capitalize; }
      .seg button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .ali { display: flex; align-items: center; justify-content: space-between; gap: 8px; font-weight: 500; color: var(--tx2); }
      .ali .grp { display: flex; gap: 4px; }
      .ali button { width: 36px; height: 30px; display: grid; place-items: center; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx2); cursor: pointer; }
      .ali button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .ali button:disabled { opacity: 0.45; cursor: not-allowed; }
      .ali svg { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; }
      .sw { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
      .sw button { display: grid; gap: 4px; justify-items: center; padding: 6px 2px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx2); font-size: 11px; cursor: pointer; text-transform: capitalize; }
      .sw button[aria-pressed="true"] { border-color: var(--acc); box-shadow: 0 0 0 1px var(--acc); color: var(--tx); }
      .sw i { display: block; width: 100%; max-width: 54px; height: 26px; border-radius: 8px; background: #202c38; border: 1px solid transparent; }
      .r-primary i { background: #20c8b0; } .r-secondary i { background: #182430; border-color: #304050; } .r-danger i { background: #182430; border-color: #f85848; }
      .s-card i { border-radius: 8px; } .s-row i { border-radius: 4px; height: 20px; margin: 3px 0; } .s-pill i { border-radius: 13px; } .s-monogram i { width: 26px; border-radius: 50%; }
      .s-card i, .s-row i, .s-pill i, .s-monogram i { background: #304050; }
      .icons { display: grid; grid-template-columns: repeat(auto-fill, minmax(38px, 1fr)); gap: 4px; max-height: 188px; overflow: auto; padding: 2px; }
      .icons button { height: 38px; display: grid; place-items: center; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf2); color: var(--tx); cursor: pointer; }
      .icons button[aria-pressed="true"] { background: var(--acc); color: var(--onacc); border-color: var(--acc); }
      .icons .sic { width: 22px; height: 22px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; }
      .slot { display: flex; align-items: center; gap: 8px; min-height: 44px; padding: 6px 10px; border: 1.5px dashed var(--line); border-radius: var(--r2); background: var(--sf2); color: var(--tx); cursor: pointer; text-align: left; width: 100%; }
      .slot.drop { border-color: var(--acc); background: var(--accs); }
      .slot b { font-weight: 600; flex: 1; min-width: 0; overflow-wrap: anywhere; }
      .chk { display: flex; gap: 8px; align-items: center; font-weight: 500; color: var(--tx); font-size: 13px; }
      .errs { display: grid; gap: 4px; }
      .err { font-size: 12px; color: var(--bad); background: var(--bads); padding: 6px 8px; border-radius: var(--r1); }
      .free.grid > .pair { height: 28px; }
      .free.grid button { height: 28px; padding: 0 8px; min-width: 0; display: flex; align-items: center; justify-content: center; text-align: center; white-space: nowrap; line-height: 1; }
      .free button { padding: 0 10px; border-radius: 15px; border: 1px solid var(--line); background: var(--sf2); color: var(--tx2); cursor: pointer; font-size: 12px; }
      .stat { display: grid; grid-template-columns: 1fr auto; gap: 2px 8px; font-size: 13px; }
      .stat span:nth-child(even) { color: var(--tx2); font-variant-numeric: tabular-nums; }
      .sic { width: 20px; height: 20px; }
      details summary { cursor: pointer; font-size: 12px; font-weight: 600; color: var(--tx2); min-height: 28px; }
      .grid4 { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-top: 8px; }
      .why { color: var(--tx3); font-size: 12px; margin: 0; }
    `,
  ];
  @property({ attribute: false }) host!: StudioHost;
  @property({ attribute: false }) assigner!: Assigner;
  private bound: EventTarget[] = [];
  private rerender = () => this.requestUpdate();
  private stopCells?: () => void;
  protected override updated(): void {
    fitCells(this.renderRoot);
    this.stopCells ??= watchCells(this, this.renderRoot);
    const now = this.host ? [this.host.state, this.host.state.store, this.assigner] : [];
    if (now.length === this.bound.length && now.every((t, i) => t === this.bound[i])) return;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = now;
    now.forEach((t) => t.addEventListener('change', this.rerender));
  }
  override disconnectedCallback(): void {
    this.stopCells?.();
    this.stopCells = undefined;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = [];
    super.disconnectedCallback();
  }
  private get st() {
    return this.host.state;
  }
  private get cfg() {
    return this.st.store.config;
  }

  // ------------------------------------------------------------ edits
  private widgetPatch(page: number, index: number, patch: Record<string, unknown>, coalesce?: string) {
    this.st.store.replace(setWidget(this.cfg, page, index, patch), coalesce ? `w${page}.${index}.${coalesce}` : undefined);
  }
  private pagePatch(page: number, patch: Record<string, unknown>, coalesce?: string) {
    this.st.store.replace(setPage(this.cfg, page, patch), coalesce ? `p${page}.${coalesce}` : undefined);
  }
  private fireSource(source: Source) {
    this.dispatchEvent(new CustomEvent('hh-inspector-pick', { detail: source, bubbles: true, composed: true }));
  }
  private errorsFor(pred: (t: NonNullable<ReturnType<typeof errorTarget>>) => boolean) {
    const list = this.st.store.errors.filter((e) => {
      const t = errorTarget(e.path);
      return t && pred(t);
    });
    return list.length ? html`<div class="errs" role="alert">${list.map((e) => html`<div class="err">${e.msg}</div>`)}</div>` : nothing;
  }

  // ------------------------------------------------------------ views
  private layerSummary() {
    const rows = this.st.rows();
    const layer = this.st.layer;
    const assigned = rows.filter((r) => r.action && r.action.kind !== 'none').length;
    const inherited = layer === DEFAULT_LAYER ? 0 : rows.filter((r) => r.source === 'default').length;
    const own = layer === DEFAULT_LAYER ? assigned : rows.filter((r) => r.source === 'activity').length;
    const free = rows.filter((r) => r.source === 'none').map((r) => r.key);
    const act = (this.cfg.activities ?? []).find((a) => a.id === layer);
    return html`<div class="pad">
      <h2>${this.st.layerName(layer)} Layer</h2>
      <div class="stat"><span>${layer === DEFAULT_LAYER ? 'Assigned Keys' : 'Overridden Here'}</span><span>${own}</span>
        ${layer === DEFAULT_LAYER ? nothing : html`<span>From Default</span><span>${inherited}</span>`}
        <span>Do Nothing</span><span>${free.length}</span></div>
      ${act?.devices?.length ? html`<div class="sec"><h3>Who Handles It</h3><div class="free cells">${act.devices.map((d) => html`<span class="chip">${(this.cfg.devices ?? []).find((x) => x.id === d.device)?.name ?? d.device}${d.input ? ` · ${displayCommand(d.input)}` : ''}</span>`)}</div></div>` : nothing}
      <div class="sec"><h3>Free Keys</h3>
        ${free.length ? html`<div class="free cells grid" style=${setStyle({ "grid-template-columns": gridStyle(free.map(displayKey), '1.5rem', FREE_CAP) })}>${orderedCells(free, (k) => k, (k) => html`<button title=${keyTitle(k)} @click=${() => this.st.set({ sel: { kind: 'key', name: k } })}>${displayKey(k)}</button>`)}</div>` : html`<p class="why">Every key does something.</p>`}
      </div>
      <p class="why">Pick a command in the palette, then a key, or drag it onto a key or the screen. Click a key or a screen widget to edit it here.</p>
    </div>`;
  }

  private alignRow(pi: number, wi: number, w: Widget, navVal: string) {
    const page = this.cfg.pages?.[pi];
    const list = !!page?.list;
    const shape = w.shape ?? (navVal || list ? 'row' : 'card');
    const ok = !!page && alignApplies({ cols: page.list ? 1 : page.cols ?? 4, rows: page.list ? page.list.rows : page.rows ?? 6, fixed: page.layout === 'fixed' }, { type: w.type, shape, icon_only: w.icon_only, x: list ? 0 : w.x ?? 0, y: list ? wi : w.y ?? 0, w: list ? 1 : w.w ?? 1, h: list ? 1 : w.h ?? 1 });
    const why = page?.layout === 'fixed' ? 'Fixed pages always read left to right' : 'Small buttons, tiles and icon-only buttons are always centred';
    return html`<div class="ali"><span id="ali-l">Align</span><div class="grp" role="group" aria-labelledby="ali-l" title=${ok ? '' : why}>${ALIGNS.map((o) => html`<button type="button" ?disabled=${!ok} aria-pressed=${(ok ? w.align ?? 'center' : 'center') === o.v} aria-label=${o.name} title=${ok ? o.name : why} @click=${() => this.widgetPatch(pi, wi, { align: o.v === 'center' ? undefined : o.v })}>${svg`<svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d=${o.d}></path></svg>`}</button>`)}</div></div>${ok ? nothing : html`<p class="why" role="note">${why}.</p>`}`;
  }

  private keyView(name: string) {
    const row = this.st.rows().find((r) => r.key === name);
    const layer = this.st.layer;
    const raw = (layer === DEFAULT_LAYER ? this.cfg.keys : (this.cfg.activities ?? []).find((a) => a.id === layer)?.keys)?.[name] ?? null;
    const state = layer === DEFAULT_LAYER ? (row?.action ? 'Assigned' : 'Does Nothing') : row?.source === 'activity' ? 'Overrides Default' : row?.source === 'default' ? 'Inherited From Default' : 'Does Nothing';
    return html`<div class="pad">
      <h2>${keyTitle(name)} <span class="chip">${this.st.layerName(layer)}</span></h2>
      <div class="muted">${state}${row?.action ? ` · ${this.host.state.actionLabel(row.action)}` : ''}</div>
      ${this.errorsFor((t) => t.kind === 'key' && t.key === name)}
      <hh-action-picker .host=${this.host} .value=${raw} .current=${row?.action ? `${this.host.state.actionLabel(row.action)}${row.source === 'default' && layer !== DEFAULT_LAYER ? ' (from Default)' : ''}` : ''} mode="key" ?inherits=${layer !== DEFAULT_LAYER} @pick=${(e: CustomEvent<Source>) => this.fireSource({ ...e.detail })}></hh-action-picker>
    </div>`;
  }

  private widgetView(pi: number, wi: number) {
    const page = this.cfg.pages?.[pi];
    const w = page?.widgets[wi];
    if (!page || !w) return this.layerSummary();
    const pp = this.st.previewOf(pi);
    const room = labelRoom(page, wi, pp?.limit ?? PAGE_BUDGET);
    const bytes = labelBytes(w.label ?? '');
    const meterPx = labelMeter(page, pp, wi);
    const isBtn = w.type === 'button';
    const navVal = typeof w['nav'] === 'string' ? (w['nav'] as string) : '';
    const picked = this.assigner.phase.name === 'picked';
    return html`<div class="pad">
      <h2>${w.type === 'label' ? 'Label' : w.type === 'tile' ? 'Tile' : 'Button'} <span class="chip">${page.title || page.id}</span></h2>
      ${this.errorsFor((t) => t.kind === 'widget' && t.page === pi && t.index === wi)}
      <label>Label <span class="cnt ${bytes >= room ? 'full' : bytes >= room - 4 ? 'warn' : ''}"><span>Shown on the Remote</span><span aria-live="polite">${bytes}/${room} B</span></span>
        <input type="text" .value=${w.label ?? ''} aria-label="Label" @input=${(e: Event) => this.widgetPatch(pi, wi, { label: trimToBytes((e.target as HTMLInputElement).value, room) }, 'label')} />
      </label>
      ${this.pxMeter(pi, wi, w, meterPx)}
      ${this.issueLine(pi, wi)}
      <div class="sec"><h3>Type</h3>
        <div class="seg" role="group" aria-label="Widget type">${(['button', 'tile', 'label'] as const).map((t) => html`<button aria-pressed=${w.type === t} @click=${() => this.setType(pi, wi, t)}>${t}</button>`)}</div>
      </div>
      ${isBtn
        ? html`<div class="sec"><h3>When Tapped</h3>
            ${navVal
              ? html`<p class="why">Goes to a page (below). Pick a different tap action to replace the page jump.</p>`
              : html`<button class="slot ${picked ? 'drop' : ''}" aria-label="When tapped: ${describeAction(this.cfg, w['action'])}. Pick a source first, then click here to assign it." @click=${() => (picked ? this.host.activateWidget(pi, wi) : this.host.openPopover())}><b>${describeAction(this.cfg, w['action'])}</b><span class="muted">${picked ? 'Drop Here' : 'Change'}</span></button>`}
            <details><summary>Choose an Action</summary><hh-action-picker .host=${this.host} .value=${w['action'] ?? null} mode="widget" @pick=${(e: CustomEvent<Source>) => this.fireSource({ ...e.detail })}></hh-action-picker></details>
          </div>`
        : nothing}
      ${w.type === 'tile'
        ? html`<label>Entity<input type="text" list="hh-tile-ents" .value=${String(w['entity'] ?? '')} placeholder="sensor.living_room_temperature" @input=${(e: Event) => this.widgetPatch(pi, wi, { entity: (e.target as HTMLInputElement).value.trim() }, 'entity')} />
            <datalist id="hh-tile-ents">${(this.st.entities ?? []).slice(0, 300).map((e) => html`<option value=${e.entity_id}>${e.name ?? ''}</option>`)}</datalist></label>
          ${String(w['entity'] ?? '') === 'sensor.choose_entity' ? html`<p class="why" role="note">Choose the entity this tile shows.</p>` : nothing}`
        : nothing}
      ${w.type === 'tile' ? this.tileTap(pi, wi, w) : nothing}
      ${w.type !== 'label'
        ? html`<div class="sec"><h3>Look</h3>
            <div class="sw" role="group" aria-label="Role">${ROLES.map((r) => html`<button class="r-${r}" aria-pressed=${(w.role ?? 'default') === r} ?disabled=${!!navVal} @click=${() => this.widgetPatch(pi, wi, { role: r === 'default' ? undefined : r })}><i></i>${r}</button>`)}</div>
            <div class="sw" role="group" aria-label="Shape">${SHAPES.map((s) => html`<button class="s-${s}" aria-pressed=${(w.shape ?? 'card') === s} @click=${() => this.widgetPatch(pi, wi, { shape: s === 'card' ? undefined : s })}><i></i>${s}</button>`)}</div>
            ${this.alignRow(pi, wi, w, navVal)}
            ${this.colourRows(pi, wi, w)}
            <span id="ico-l">Icon</span>
            <hh-icon-picker aria-labelledby="ico-l" .value=${w.icon && w.icon !== 'none' ? w.icon : ''} .command=${String((w['action'] as { command?: string } | undefined)?.command ?? '')} .label=${w.label ?? ''}
              @hh-icon=${(e: CustomEvent<string | undefined>) => this.widgetPatch(pi, wi, { icon: e.detail, ...(e.detail && !w.color && stockIconByName(e.detail)?.brand ? { color: stockIconByName(e.detail)?.brand } : {}) })}></hh-icon-picker>
            <label class="chk"><input type="checkbox" .checked=${wireIconOnly(page, pp, wi)} @change=${(e: Event) => this.widgetPatch(pi, wi, { icon_only: (e.target as HTMLInputElement).checked })} /> Icon only (the label is kept but not drawn)</label>
          </div>`
        : nothing}
      ${w.type === 'label' ? html`<div class="sec"><h3>Look</h3>${this.colourRows(pi, wi, w)}</div>` : nothing}
      ${this.colourCost(pi, page, w)}
      ${isBtn
        ? html`<label>Tap goes to page
            <select .value=${navVal} @change=${(e: Event) => this.setNav(pi, wi, (e.target as HTMLSelectElement).value)}>
              <option value="">No</option>
              <option value="next" ?selected=${navVal === 'next'} ?disabled=${pi >= (this.cfg.pages ?? []).length - 1}>Next page${pi >= (this.cfg.pages ?? []).length - 1 ? ' (this is the last page)' : ''}</option>
              <option value="prev" ?selected=${navVal === 'prev'} ?disabled=${pi === 0}>Previous page${pi === 0 ? ' (this is the first page)' : ''}</option>
              <option value="home" ?selected=${navVal === 'home'} ?disabled=${!(this.cfg.pages ?? []).some((p) => p.kind === 'home')}>Home${(this.cfg.pages ?? []).some((p) => p.kind === 'home') ? '' : ' (no Home page yet)'}</option>
              ${(this.cfg.pages ?? []).map((p, i) => (i === pi ? nothing : html`<option value=${p.id} ?selected=${navVal === p.id}>${p.title || p.id}</option>`))}
            </select></label>`
        : nothing}
      <details><summary>More</summary>
        <div class="grid4">
          ${(['x', 'y', 'w', 'h'] as const).map((k) => html`<label>${k}<input type="number" min="0" max="16" .value=${String((w[k] as number | undefined) ?? (k === 'w' || k === 'h' ? 1 : 0))} ?disabled=${isList(page)} @change=${(e: Event) => this.numEdit(pi, wi, k, Number((e.target as HTMLInputElement).value))} /></label>`)}
        </div>
        <p class="why">Id <code>${w.id ?? '(none)'}</code> · ${isList(page) ? 'list pages place rows themselves' : `grid ${gridOf(page).cols}x${gridOf(page).rows}`}</p>
      </details>
      <div class="seg"><button @click=${() => this.host.removeWidget(pi, wi)}>Delete Widget</button></div>
    </div>`;
  }

  private colourRows(pi: number, wi: number, w: Widget) {
    void import('./hh-colour-field'); // the colour field and its maths load with the first widget that shows them
    const theme = activeTheme(this.cfg).theme;
    const kind = w.type === 'tile' ? 'tile' : w.type === 'label' ? 'label' : 'button';
    const look = (o: { state?: 'on' | 'value' }) => widgetColors({ kind, role: w.role ?? 'default' }, o, theme);
    const dflt = look({ state: 'value' });
    const on = look({ state: 'on' });
    const set = (key: 'color' | 'on_color') => (e: CustomEvent<{ value: string | undefined; coalesce: boolean }>) =>
      this.widgetPatch(pi, wi, { [key]: e.detail.value }, e.detail.coalesce ? `col-${key}` : undefined);
    const add = (key: 'color' | 'on_color') => (e: CustomEvent<AddPaletteDetail>) => void this.addToPalette(pi, wi, key, e.detail);
    return html`<hh-colour-field .theme=${theme} .value=${w.color} heading=${w.type === 'label' ? 'Text Colour' : w.type === 'tile' ? 'Colour' : 'Colour'}
        defaultLabel=${w.type === 'label' ? 'Default' : 'Default (role)'} .defaultLook=${w.type === 'label' ? [theme.tokens.bg, theme.tokens.tx2] : [dflt.fill, dflt.ink]}
        @colour=${set('color')} @add-palette=${add('color')}></hh-colour-field>
      ${w.type === 'tile'
        ? html`<hh-colour-field .theme=${theme} .value=${w.on_color} heading="When on" defaultLabel="Default (warm)" .defaultLook=${[on.fill, on.ink]}
            @colour=${set('on_color')} @add-palette=${add('on_color')}></hh-colour-field>`
        : nothing}`;
  }
  /** Adds a colour to the active user theme's palette (making one of the user's own first when a preset is active) and points the widget at it. */
  private async addToPalette(pi: number, wi: number, key: 'color' | 'on_color', d: AddPaletteDetail) {
    const E = await import('../lib/theme-edit');
    const cfg = this.cfg;
    const name = activeName(cfg);
    const slot = PALETTE_NAMES[d.slot] as string;
    let next;
    if (name in PRESETS) {
      const mine = E.uniqueThemeName(cfg, `my_${name}`);
      const ok = await confirmAction(this, {
        title: 'Make a theme of your own?',
        body: `"${PRESETS[name]?.name}" is built in and cannot be changed. This creates your theme "${mine}" on top of it, with ${slot} set to ${d.hex}, and switches the remote to it.`,
        ok: 'Create Theme and Add',
      });
      if (!ok) return;
      next = E.selectTheme(E.createTheme(cfg, mine, name, { palette: { [String(d.slot + 1)]: { fill: d.hex } } }), mine);
    } else {
      const ok = await confirmAction(this, {
        title: `Replace ${slot} in "${name}"?`,
        body: `Every widget that uses ${slot} will change to ${d.hex}.`,
        ok: `Replace ${slot}`,
      });
      if (!ok) return;
      next = E.setPaletteFill(cfg, name, d.slot, d.hex);
    }
    this.st.store.replace(setWidget(next, pi, wi, { [key]: slot }));
  }
  /** The colour bytes of this widget and the compiler's over-budget message for its page. */
  private colourCost(pi: number, page: Page, w: Widget) {
    const cb = colourBytes(w);
    const pe = this.st.store.pages[pi]?.error;
    const err = pe && /colour/i.test(pe) ? pe : null;
    const total = page.widgets.reduce((n, x) => n + colourBytes(x), 0);
    if (!cb && !err) return nothing;
    return html`${cb ? html`<p class="why" id="colour-cost">Colour uses ${cb} B here (${total} B on this page). A palette name costs 1 B, a custom colour 2 B more.</p>` : nothing}
      ${err ? html`<div class="errs" role="alert"><div class="err">${err}</div></div>` : nothing}`;
  }

  /** "When tapped" for a tile: automatic (toggle / turn on / press by domain), toggle, an action, or nothing. */
  private tileTap(pi: number, wi: number, w: Widget) {
    const st = this.st;
    const entity = String(w['entity'] ?? '');
    const auto = tileDefaultTap(entity);
    const tap = w['tap'];
    const mode = tap === undefined ? 'auto' : tap === 'toggle' ? 'toggle' : tap === 'none' ? 'none' : 'action';
    if (!st.haWritable)
      return html`<div class="sec"><h3>When Tapped</h3><p class="why" role="note">Disabled: this add-on version cannot store tile taps yet (it reports ha_actions: off). Tiles only show state.</p></div>`;
    const set = (v: unknown) => this.widgetPatch(pi, wi, { tap: v });
    return html`<div class="sec"><h3>When Tapped</h3>
      <div class="seg" role="group" aria-label="When tapped">
        <button aria-pressed=${mode === 'auto'} @click=${() => set(undefined)}>Automatic</button>
        <button aria-pressed=${mode === 'toggle'} ?disabled=${!auto} title=${auto ? '' : 'Nothing to toggle for this kind of entity'} @click=${() => set('toggle')}>Toggle</button>
        <button aria-pressed=${mode === 'action'} @click=${() => { if (mode !== 'action') set({ ha: { service: auto ?? 'homeassistant.turn_on', target: { entity_id: entity } } }); }}>Action</button>
        <button aria-pressed=${mode === 'none'} @click=${() => set('none')}>Nothing</button>
      </div>
      <p class="why">${mode === 'auto' ? (auto ? `Automatic: a tap runs ${auto} on ${entity}.` : 'Automatic: this kind of entity has no default tap, so a tap does nothing.') : mode === 'toggle' ? `A tap runs ${auto} on ${entity}.` : mode === 'none' ? 'A tap does nothing.' : `A tap runs ${describeAction(this.cfg, tap)}.`}</p>
      ${mode === 'action' ? html`<hh-action-picker .host=${this.host} .value=${tap ?? null} mode="widget" @pick=${(e: CustomEvent<Source>) => { const a = sourceAction(e.detail); this.widgetPatch(pi, wi, { tap: e.detail.kind === 'none' ? 'none' : e.detail.kind === 'default' ? undefined : a }); }}></hh-action-picker>` : nothing}
    </div>`;
  }

  private numEdit(pi: number, wi: number, k: 'x' | 'y' | 'w' | 'h', v: number) {
    const page = this.cfg.pages?.[pi];
    const w = page?.widgets[wi];
    if (!page || !w) return;
    const b = { x: Number(w.x ?? 0), y: Number(w.y ?? 0), w: Number(w.w ?? 1), h: Number(w.h ?? 1), [k]: v };
    this.host.setBox(pi, wi, b);
  }
  private setNav(pi: number, wi: number, nav: string) {
    this.widgetPatch(pi, wi, nav ? { nav, action: undefined, role: undefined } : { nav: undefined, action: { none: true } });
  }
  private setType(pi: number, wi: number, type: string) {
    const w = this.cfg.pages?.[pi]?.widgets[wi];
    if (!w || w.type === type) return;
    if (type === 'label') this.widgetPatch(pi, wi, { type, action: undefined, entity: undefined, nav: undefined, id: undefined, icon: undefined, role: undefined, shape: undefined, icon_only: undefined, align: undefined });
    else if (type === 'tile') this.widgetPatch(pi, wi, { type, action: undefined, nav: undefined, entity: 'sensor.choose_entity', id: w.id ?? `tile_${wi + 1}` });
    else this.widgetPatch(pi, wi, { type, entity: undefined, on_color: undefined, action: { none: true }, id: w.id ?? `btn_${wi + 1}` });
  }

  private issueOf(pi: number, wi: number) {
    return widgetIssues(collectIssues(this.st.store.errors, this.st.store.warnings, this.st.store.checks, this.cfg), pi).get(wi);
  }
  /** The one issue on this widget (text from issueText) and, for a label that can be shortened, its fix. */
  private issueLine(pi: number, wi: number) {
    const issue = this.issueOf(pi, wi);
    if (!issue) return nothing;
    const fix = issue.fix;
    return html`<div class="issue ${issue.level}"><div>${issue.text}</div>${fix ? html`<button type="button" class="chip" @click=${() => this.widgetPatch(pi, wi, { label: fix.value }, 'label')}>${fix.label}</button>` : nothing}</div>`;
  }
  /** The label's pixel meter (what the remote can show, not bytes) and one-tap shorter forms when it is over. */
  private pxMeter(pi: number, wi: number, w: Widget, m: ReturnType<typeof labelMeter>) {
    if (!m) return nothing;
    const cmd = String((w['action'] as { command?: string } | undefined)?.command ?? '');
    const label = w.label ?? '';
    const options = this.issueOf(pi, wi)?.fix ? [] : [...new Set([abbreviate(label), cmd ? abbreviate(displayName(cmd)) : ''])].filter((o) => o && o !== label && textPx(o, m.font) <= m.px);
    const state = m.over > 0 ? 'full' : m.px - m.used <= 6 ? 'warn' : '';
    return html`<div class="pxm">
      <div class="cnt ${state}"><span>Fits the Remote</span><span aria-live="polite">${m.used} / ${m.px} px · ${m.font}</span></div>
      ${m.over > 0 && options.length ? html`<div class="chips" role="group" aria-label="Shorter labels">${options.map((o) => html`<button type="button" class="chip" @click=${() => this.widgetPatch(pi, wi, { label: o }, 'label')}>${o}</button>`)}</div>` : nothing}
    </div>`;
  }
  private pageView(pi: number) {
    const page = this.cfg.pages?.[pi] as (Page & { activity?: string }) | undefined;
    if (!page) return this.layerSummary();
    const pp = this.st.previewOf(pi);
    const bytes = pp?.bytes ?? pageBytes(page);
    const limit = pp?.limit ?? PAGE_BUDGET;
    const titleB = labelBytes(page.title ?? '');
    return html`<div class="pad">
      <h2>Page <span class="chip">${page.id}</span></h2>
      ${this.errorsFor((t) => t.kind === 'page' && t.page === pi)}
      ${pp?.error ? html`<div class="errs" role="alert"><div class="err">${pp.error}</div></div>` : nothing}
      ${meter('Page size', `${bytes} / ${limit} B`, bytes, limit)}
      ${meter('Widgets', `${page.widgets.length} / ${pp?.widget_limit ?? 32}`, page.widgets.length, pp?.widget_limit ?? 32)}
      <label>Title <span class="cnt"><span>Top Bar</span><span>${titleB}/24 B</span></span>
        <input type="text" .value=${page.title ?? ''} @input=${(e: Event) => this.pagePatch(pi, { title: trimToBytes((e.target as HTMLInputElement).value, 24) }, 'title')} />
      </label>
      <div class="sec"><h3>Kind</h3>
        <div class="seg" role="group" aria-label="Page kind">
          ${(['normal', 'home', 'favorites'] as const).map((k) => html`<button aria-pressed=${(page.kind ?? 'normal') === k} @click=${() => this.setKind(pi, k)}>${k}</button>`)}
        </div>
      </div>
      <label>Show when this activity starts
        <select .value=${page.activity ?? ''} @change=${(e: Event) => this.pagePatch(pi, { activity: (e.target as HTMLSelectElement).value || undefined })}>
          <option value="">Never (Manual)</option>
          ${(this.cfg.activities ?? []).map((a) => html`<option value=${a.id} ?selected=${page.activity === a.id}>${dn(a.name, a.id)}</option>`)}
        </select></label>
      <label class="chk"><input type="checkbox" .checked=${!!page.back} @change=${(e: Event) => this.pagePatch(pi, { back: (e.target as HTMLInputElement).checked || undefined })} /> Back Chevron (Exit Goes Home)</label>
      ${page.list ? html`<label>List Rows Per Screen<input type="number" min="1" max="16" .value=${String(page.list.rows)} @change=${(e: Event) => this.pagePatch(pi, { list: { rows: Math.max(1, Math.min(16, Number((e.target as HTMLInputElement).value) || 6)) } })} /></label>` : nothing}
      ${!page.list ? html`<div class="grid4"><label>Cols<input type="number" min="1" max="16" .value=${String(page.cols ?? 4)} @change=${(e: Event) => this.pagePatch(pi, { cols: Math.max(1, Math.min(16, Number((e.target as HTMLInputElement).value) || 4)) })} /></label><label>Rows<input type="number" min="1" max="16" .value=${String(page.rows ?? 6)} @change=${(e: Event) => this.pagePatch(pi, { rows: Math.max(1, Math.min(16, Number((e.target as HTMLInputElement).value) || 6)) })} /></label></div>` : html`<p class="why">A list page splits into screens of ${page.list.rows} rows; Up/Down pages through them on the remote.</p>`}
    </div>`;
  }
  private setKind(pi: number, kind: string) {
    const pages = this.cfg.pages ?? [];
    if (kind !== 'normal' && pages.some((p, i) => i !== pi && p.kind === kind)) {
      this.st.store.toast(`Only one ${kind} page is allowed. Change the other one first.`, 'error');
      return;
    }
    this.pagePatch(pi, { kind: kind === 'normal' ? undefined : kind });
  }

  protected override render() {
    if (!this.host) return nothing;
    const sel = this.st.sel;
    void LABEL_MAX;
    void KEYS;
    if (sel?.kind === 'key') return this.keyView(sel.name);
    if (sel?.kind === 'widget') return this.widgetView(sel.page, sel.index);
    if (sel?.kind === 'page') return this.pageView(this.st.page);
    return this.layerSummary();
  }
}
