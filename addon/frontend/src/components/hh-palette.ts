import { LitElement, css, html, nothing, type TemplateResult } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { Assigner } from '../lib/assigner';
import { commandNames } from '../lib/bt';
import { setStyle } from '../lib/set-style';
import { displayCommand, gridStyle } from '../lib/key-names';
import { stockIconSvg } from '../lib/icon-shapes';
import { commandIconId } from '../lib/device-art';
import { chipStyles, shared } from '../lib/styles';
import { permissions } from '../lib/permissions';
import { HA_DOMAINS, type PaletteTab, type StudioHost } from '../lib/studio-state';
import { GROUPS, HA_ENTITY_ID, deviceChips, layerKeys, pretty, sourceKey, type Source } from '../lib/studio-model';
import { stockIconByName } from '../lib/stock-icons';
import type { Config } from '../lib/model';

const TABS: [PaletteTab, string, string][] = [['devices', 'Devices', 'tv'], ['activities', 'Activities', 'play'], ['ha', 'Home Assistant', 'scene'], ['widgets', 'Widgets', 'list']];

/** Left pane: sources to drag onto keys and the screen, or to click and then click a target. */
@customElement('hh-palette')
export class HhPalette extends LitElement {
  static override styles = [
    shared,
    chipStyles,
    css`
      :host { display: flex; flex-direction: column; min-height: 0; height: 100%; background: var(--sf); }
      .head { padding: 10px 12px 8px; display: grid; gap: 8px; border-bottom: 1px solid var(--line); }
      .tabs { display: flex; align-items: center; gap: 2px; background: var(--sf2); border-radius: var(--r1); padding: 2px; }
      .tabs button { position: relative; display: flex; align-items: center; justify-content: center; flex: 0 0 36px; height: 32px; padding: 0; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); cursor: pointer; }
      .tabs button:hover { background: var(--sf3); }
      .tabs button[aria-selected="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.12); }
      .tabs button .sic { width: 17px; height: 17px; }
      .tabs button::after { content: attr(aria-label); position: absolute; top: calc(100% + 6px); left: 0; z-index: 5; padding: 3px 8px; border-radius: var(--r1); background: var(--tx); color: var(--bg); font-size: 11.5px; font-weight: 600; white-space: nowrap; pointer-events: none; opacity: 0; transition: opacity 80ms; }
      .tabs button:hover::after, .tabs button:focus-visible::after { opacity: 1; }
      .tabs .tn { flex: 1 0 auto; padding: 0 6px; font-size: 12px; font-weight: 650; color: var(--tx); white-space: nowrap; }
      .tabs .tn.off { position: absolute; visibility: hidden; pointer-events: none; }
      details.typed-d { margin: 10px 0 0; border: 0; border-top: 1px solid var(--line); border-radius: 0; background: transparent; padding-top: 4px; }
      .typed-d summary { min-height: 32px; padding: 0 2px; font-weight: 600; font-size: 12px; color: var(--tx2); border-radius: var(--r1); }
      input[type="search"], .typed input { height: 34px; width: 100%; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; }
      .body { overflow: auto; scrollbar-gutter: stable; padding: 8px 12px 16px; flex: 1; min-height: 0; }
      details { border: 1px solid var(--line); border-radius: var(--r2); margin: 8px 0; background: var(--sf); }
      summary { display: flex; align-items: center; gap: 8px; min-height: 40px; padding: 0 10px; cursor: pointer; font-weight: 650; list-style: none; border-radius: var(--r2); }
      summary::-webkit-details-marker { display: none; }
      summary .sic { width: 20px; height: 20px; color: var(--acc); }
      summary > span { min-width: 0; overflow-wrap: anywhere; }
      summary small { margin-left: auto; flex: none; white-space: nowrap; color: var(--tx3); font-weight: 500; font-size: 11px; }
      .grp { padding: 0 10px 10px; }
      .grp h2 { margin: 8px 0 5px; font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; }
      .hab { flex: none; font-style: normal; font-size: 10px; font-weight: 700; padding: 1px 5px; border-radius: 8px; border: 1px solid var(--line); color: var(--tx2); background: var(--sf); }
      .empty { color: var(--tx3); font-size: 12.5px; padding: 12px 2px; }
      .big { display: grid; gap: 6px; margin-top: 8px; }
      .prim { display: flex; align-items: center; gap: 10px; min-height: 52px; padding: 0 12px; border-radius: var(--r2); border: 1px solid var(--line); background: var(--sf2); color: var(--tx); cursor: grab; text-align: left; touch-action: pan-y; user-select: none; -webkit-user-select: none; font-weight: 600; }
      .prim[aria-pressed="true"] { background: var(--acc); color: var(--onacc); border-color: var(--acc); }
      .prim small { display: block; font-weight: 400; color: var(--tx3); font-size: 11.5px; }
      .prim[aria-pressed="true"] small { color: inherit; }
      .prim .sic { width: 22px; height: 22px; color: var(--acc); }
      .prim[aria-pressed="true"] .sic { color: inherit; }
      .typed { display: grid; gap: 6px; margin-top: 8px; }
      .hint { color: var(--tx3); font-size: 11.5px; margin: 6px 2px 0; }
    `,
  ];
  @property({ attribute: false }) host!: StudioHost;
  @property({ attribute: false }) assigner!: Assigner;
  @state() private typed = '';
  private bound: EventTarget[] = [];
  private rerender = () => this.requestUpdate();
  private ro?: ResizeObserver;
  /** the tab's name shows in full or not at all: hidden when it does not fit beside the icons (the icon, tooltip and aria-label name it) */
  private fitName = () => {
    const tabs = this.renderRoot.querySelector<HTMLElement>('.tabs');
    const tn = tabs?.querySelector<HTMLElement>('.tn');
    const last = tabs?.querySelector<HTMLElement>('button:last-of-type');
    if (!tabs || !tn || !last) return;
    const fits = last.offsetLeft + last.offsetWidth + 2 + tn.scrollWidth + 2 <= tabs.clientWidth;
    tn.classList.toggle('off', !fits);
  };
  protected override updated(): void {
    const tabs = this.renderRoot.querySelector('.tabs');
    if (tabs && !this.ro) {
      this.ro = new ResizeObserver(this.fitName);
      this.ro.observe(tabs);
    }
    this.fitName();
    const now = this.host ? [this.host.state, this.host.state.store, this.assigner] : [];
    if (now.length === this.bound.length && now.every((t, i) => t === this.bound[i])) return;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = now;
    now.forEach((t) => t.addEventListener('change', this.rerender));
  }
  override connectedCallback(): void {
    super.connectedCallback();
    window.addEventListener('focus', this.onFocus);
  }
  /** back in the window after a while: refresh the entity list (a 304 when nothing changed) */
  private onFocus = () => {
    if (this.host?.state.tab === 'ha') this.host.state.refreshHa(60000);
  };
  override disconnectedCallback(): void {
    window.removeEventListener('focus', this.onFocus);
    this.ro?.disconnect();
    this.ro = undefined;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = [];
    super.disconnectedCallback();
  }

  private get st() {
    return this.host.state;
  }
  private cfg(): Config {
    return this.st.store.config;
  }
  /** `device:command` / `device:input:x` / `activity:x` used by keys of the current layer */
  private usedKeys(): Set<string> {
    const used = new Set<string>();
    for (const a of Object.values(layerKeys(this.cfg(), this.st.layer))) {
      const x = a as Record<string, unknown>;
      if (x?.['device']) used.add(x['command'] ? `${x['device']}:${x['command']}` : `${x['device']}:input:${x['input']}`);
      else if (x?.['activity']) used.add(`activity:${x['activity']}`);
      else if (x?.['ha']) used.add(`ha:${(x['ha'] as { target?: { entity_id?: string } }).target?.entity_id}`);
    }
    return used;
  }
  private down(e: PointerEvent, source: Source) {
    if (e.button !== 0) return;
    this.dispatchEvent(new CustomEvent('hh-chip-down', { detail: { source, ev: e }, bubbles: true, composed: true }));
  }
  private key(e: KeyboardEvent, source: Source) {
    if (e.key === ' ' || e.key === 'Enter') {
      e.preventDefault();
      e.stopPropagation(); // the studio's window handler would read this Enter as "drop"

      this.dispatchEvent(new CustomEvent('hh-chip-pick', { detail: { source }, bubbles: true, composed: true }));
    }
  }
  private pressed(s: Source): boolean {
    const cur = this.assigner.source;
    return !!cur && sourceKey(cur) === sourceKey(s);
  }
  private chip(raw: string, source: Source, used: boolean, extra: TemplateResult | typeof nothing = nothing) {
    const label = source.kind === 'activity' ? raw : displayCommand(raw);
    return html`<button class="chip2 ${used ? 'used' : ''}" aria-pressed=${this.pressed(source)} title=${used ? `${label} (already on a key of this layer)` : label}
      @pointerdown=${(e: PointerEvent) => this.down(e, source)} @keydown=${(e: KeyboardEvent) => this.key(e, source)} @click=${(e: MouseEvent) => e.preventDefault()}>${extra}<span>${label}</span></button>`;
  }
  private match(...s: string[]): boolean {
    const q = this.st.query.trim().toLowerCase();
    return !q || s.some((x) => x.toLowerCase().includes(q));
  }

  private devices() {
    const used = this.usedKeys();
    const devs = (this.cfg().devices ?? []).filter((d) => this.match(d.name ?? '', d.id, ...commandNames(d), ...Object.keys(d.inputs ?? {})));
    if (!devs.length) return html`<p class="empty">${this.st.query ? 'Nothing matches.' : 'No devices yet. Import YAML or add a device first.'}</p>`;
    return devs.map((d) => {
      const g = deviceChips(d);
      const q = this.st.query.trim();
      const cards = GROUPS.map((grp) => {
        const chips = g[grp].filter((c) => !q || this.match(c.name, d.name ?? d.id, d.id));
        return chips.length
          ? html`<h2>${pretty(grp)}</h2><div class="cells" style=${setStyle({ "grid-template-columns": gridStyle(chips.map((c) => displayCommand(c.name))) })} role="group" aria-label=${`${d.name ?? d.id} ${grp}`}>
              ${chips.map((c) => this.chip(c.name, { kind: c.kind, device: c.device, name: c.name }, used.has(c.kind === 'input' ? `${d.id}:input:${c.name}` : `${d.id}:${c.name}`), html`${stockIconSvg(commandIconId(c.name, c.group))}${d.kind === 'ha' ? html`<i class="hab" title="Home Assistant action">HA</i>` : nothing}`))}
            </div>`
          : nothing;
      });
      return html`<details open><summary>${d.icon && stockIconByName(d.icon) ? stockIconSvg(stockIconByName(d.icon)?.id ?? 1) : stockIconSvg(1)}<span title=${d.name ?? d.id}>${d.name ?? d.id}</span>${d.kind === 'ha' ? html`<i class="hab" title="Home Assistant device">HA</i>` : nothing}<small>${commandNames(d).length} cmd</small></summary><div class="grp">${cards}</div></details>`;
    });
  }
  private activities() {
    const used = this.usedKeys();
    const acts = (this.cfg().activities ?? []).filter((a) => this.match(a.name ?? '', a.id));
    return html`
      <div class="cells">
        ${acts.map((a) => this.chip(a.name ?? a.id, { kind: 'activity', id: a.id }, used.has(`activity:${a.id}`), stockIconSvg(stockIconByName('play')?.id ?? 26, 'sic')))}
        ${this.match('all off', 'power off') ? this.chip('All off', { kind: 'alloff' }, used.has('activity:off'), stockIconSvg(stockIconByName('power')?.id ?? 7, 'sic')) : nothing}
      </div>
      ${acts.length ? nothing : html`<p class="empty">${this.st.query ? 'Nothing matches.' : 'No activities in this config.'}</p>`}
      <p class="hint">Drop one on a key to start it from that key, or on the screen to make a start button.</p>`;
  }
  private qTimer?: ReturnType<typeof setTimeout>;
  /** The palette re-renders hundreds of chips per query: wait for a pause in typing. */
  private search(q: string) {
    clearTimeout(this.qTimer);
    this.qTimer = setTimeout(() => this.st.set({ query: q }), 120);
  }
  private ha() {
    const st = this.st;
    void import('./hh-ha-browser'); // lazy: only this tab needs the navigator
    void st.loadHa();
    const typedSource: Source | null = HA_ENTITY_ID.test(this.typed) ? { kind: 'entity', entity_id: this.typed } : null;
    const typed = html`<div class="typed">
      <input type="text" aria-label="Entity id" placeholder="light.living_room" .value=${this.typed} @input=${(e: Event) => (this.typed = (e.target as HTMLInputElement).value.trim())} />
      ${typedSource ? html`<div class="cells">${this.chip(this.typed, typedSource, false)}</div>` : html`<span class="hint">Type an entity id to get a chip you can drag.</span>`}
    </div>`;
    const used = new Map(permissions(this.cfg()).entities.map((e) => [e.id, e.services]));
    const cur = this.assigner.source;
    return html`<hh-ha-browser .entities=${st.entities} .loaded=${st.haLoaded} .loading=${st.haLoading} .reason=${st.haReason} .registries=${st.haRegistries} .lookups=${st.haLookups} .truncated=${st.haTruncated}
      .domains=${HA_DOMAINS} .used=${used} .pressed=${cur?.kind === 'entity' ? cur.entity_id : ''} .diag=${st.haDiag}
      @hh-ha-retry=${() => void st.loadHa(true)} @hh-ha-refresh=${() => void st.loadHa(true)} @hh-ha-diag=${(e: CustomEvent<boolean>) => st.setHaDiag(e.detail)}></hh-ha-browser>
      <details class="typed-d" ?open=${st.haLoaded && st.entities === null}><summary>Use an entity id</summary>${typed}</details>`;
  }
  private widgets() {
    const prims: [Source, string, string, string][] = [
      [{ kind: 'widget', type: 'button' }, 'Button', 'Runs an action when tapped', 'plus'],
      [{ kind: 'widget', type: 'tile' }, 'Tile', 'Shows a Home Assistant entity', 'switch'],
      [{ kind: 'widget', type: 'label' }, 'Label', 'Section caption, no action', 'list'],
    ];
    return html`<div class="big">${prims
      .filter(([, n]) => this.match(n))
      .map(([src, name, note, ic]) => html`<button class="prim" aria-pressed=${this.pressed(src)} @pointerdown=${(e: PointerEvent) => this.down(e, src)} @keydown=${(e: KeyboardEvent) => this.key(e, src)} @click=${(e: MouseEvent) => e.preventDefault()}>${stockIconSvg(stockIconByName(ic)?.id ?? 15)}<span>${name}<small>${note}</small></span></button>`)}
      ${this.match('list', 'device list')
        ? html`<button class="prim" @click=${() => this.dispatchEvent(new CustomEvent('hh-add-page', { detail: 'devices', bubbles: true, composed: true }))}>${stockIconSvg(stockIconByName('list')?.id ?? 25)}<span>List<small>Adds a Device list page</small></span></button>`
        : nothing}
    </div><p class="hint">Drop a widget on an empty cell of the screen.</p>`;
  }

  protected override render() {
    if (!this.host) return nothing;
    const st = this.st;
    return html`
      <div class="head">
        <div class="tabs" role="tablist" aria-label="Palette">
          ${TABS.map(([id, label, ic]) => html`<button role="tab" id=${`pt-${id}`} aria-selected=${st.tab === id} aria-label=${label} aria-controls="pbody" @click=${() => { st.set({ tab: id }); if (id === 'ha' && st.haLoaded) void st.loadHa(true); }}>${stockIconSvg(stockIconByName(ic)?.id ?? 13)}</button>`)}
          <span class="tn" aria-hidden="true">${TABS.find(([id]) => id === st.tab)?.[1]}</span>
        </div>
        ${st.tab === 'ha' ? nothing : html`<input type="search" aria-label="Search the palette" placeholder="Search the palette" .value=${st.query} @input=${(e: Event) => this.search((e.target as HTMLInputElement).value)} />`}
      </div>
      <div class="body" id="pbody" role="tabpanel" aria-labelledby=${`pt-${st.tab}`}>
        ${st.tab === 'devices' ? this.devices() : st.tab === 'activities' ? this.activities() : st.tab === 'ha' ? this.ha() : this.widgets()}
      </div>`;
  }
}
