import { LitElement, css, html, nothing, type TemplateResult } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { Assigner } from '../lib/assigner';
import { stockIconSvg } from '../lib/icon-shapes';
import { commandIconId } from '../lib/device-art';
import { shared } from '../lib/styles';
import { HA_DOMAINS, type PaletteTab, type StudioHost } from '../lib/studio-state';
import { GROUPS, HA_ENTITY_ID, deviceChips, entityDomain, layerKeys, pretty, sourceKey, type Source } from '../lib/studio-model';
import { stockIconByName } from '../lib/stock-icons';
import type { Config } from '../lib/model';

const TABS: [PaletteTab, string][] = [['devices', 'Devices'], ['activities', 'Activities'], ['home', 'Home'], ['widgets', 'Widgets']];
const DOMAIN_ICON: Record<string, string> = { light: 'bulb', switch: 'switch', fan: 'fan', cover: 'blinds', climate: 'thermo', scene: 'scene', script: 'play', media_player: 'tv', lock: 'lock' };

/** Left pane: sources to drag onto keys and the screen, or to click and then click a target. */
@customElement('hh-palette')
export class HhPalette extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; min-height: 0; height: 100%; background: var(--sf); }
      .head { padding: 10px 12px 8px; display: grid; gap: 8px; border-bottom: 1px solid var(--line); }
      .tabs { display: grid; grid-template-columns: repeat(4, 1fr); gap: 2px; background: var(--sf2); border-radius: var(--r1); padding: 2px; }
      .tabs button { height: 30px; padding: 0 2px; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); font-size: 11.5px; white-space: nowrap; font-weight: 600; cursor: pointer; }
      .tabs button[aria-selected="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.12); }
      input[type="search"], .typed input { height: 34px; width: 100%; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; }
      .body { overflow: auto; padding: 8px 12px 16px; flex: 1; min-height: 0; }
      details { border: 1px solid var(--line); border-radius: var(--r2); margin: 8px 0; background: var(--sf); }
      summary { display: flex; align-items: center; gap: 8px; min-height: 40px; padding: 0 10px; cursor: pointer; font-weight: 650; list-style: none; border-radius: var(--r2); }
      summary::-webkit-details-marker { display: none; }
      summary .sic { width: 20px; height: 20px; color: var(--acc); }
      summary small { margin-left: auto; color: var(--tx3); font-weight: 500; font-size: 11px; }
      .grp { padding: 0 10px 10px; }
      .grp h4 { margin: 8px 0 5px; font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; }
      .chips { display: flex; flex-wrap: wrap; gap: 5px; }
      .chip2 { display: inline-flex; align-items: center; gap: 6px; min-height: 28px; max-width: 100%; padding: 0 10px; border-radius: 14px; border: 1px solid var(--line); background: var(--sf2); color: var(--tx); font-size: 12.5px; cursor: grab; touch-action: pan-y; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
      .chip2:hover { background: var(--sf3); }
      .chip2 span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .chip2.used { border-color: var(--acc); box-shadow: inset 0 0 0 1px var(--acc); }
      .chip2[aria-pressed="true"] { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
      .chip2 .sic { width: 15px; height: 15px; flex: none; }
      .ent { display: flex; align-items: center; gap: 8px; width: 100%; min-height: 36px; padding: 4px 8px; border-radius: var(--r1); border: 1px solid transparent; background: transparent; color: var(--tx); text-align: left; cursor: grab; touch-action: pan-y; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
      /* a few hundred Home Assistant entities: rows outside the viewport are not laid out or painted */
      .ent { content-visibility: auto; contain-intrinsic-size: auto 40px; }
      .ent:hover { background: var(--sf2); }
      .ent[aria-pressed="true"] { background: var(--acc); color: var(--onacc); }
      .ent .sic { width: 18px; height: 18px; color: var(--tx2); }
      .ent .t { min-width: 0; flex: 1; display: grid; }
      .ent .t b { font-weight: 550; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .ent .t i { font-style: normal; font-size: 11px; color: var(--tx3); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .ent[aria-pressed="true"] .t i, .ent[aria-pressed="true"] .sic { color: inherit; }
      .st { font-size: 11px; padding: 1px 7px; border-radius: 9px; background: var(--sf3); color: var(--tx2); }
      .st.on { background: var(--irs); color: var(--ir); }
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
      @media (max-width: 720px) { .chip2 { min-height: 36px; } .ent { min-height: 44px; } }
    `,
  ];
  @property({ attribute: false }) host!: StudioHost;
  @property({ attribute: false }) assigner!: Assigner;
  @state() private typed = '';
  private bound: EventTarget[] = [];
  private rerender = () => this.requestUpdate();
  protected override updated(): void {
    const now = this.host ? [this.host.state, this.host.state.store, this.assigner] : [];
    if (now.length === this.bound.length && now.every((t, i) => t === this.bound[i])) return;
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = now;
    now.forEach((t) => t.addEventListener('change', this.rerender));
  }
  override disconnectedCallback(): void {
    this.bound.forEach((t) => t.removeEventListener('change', this.rerender));
    this.bound = [];
    if (this.poll) clearInterval(this.poll);
    super.disconnectedCallback();
  }
  private poll: ReturnType<typeof setInterval> | null = null;

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
  private chip(label: string, source: Source, used: boolean, extra: TemplateResult | typeof nothing = nothing) {
    return html`<button class="chip2 ${used ? 'used' : ''}" aria-pressed=${this.pressed(source)} title=${used ? `${label} (already on a key of this layer)` : label}
      @pointerdown=${(e: PointerEvent) => this.down(e, source)} @keydown=${(e: KeyboardEvent) => this.key(e, source)} @click=${(e: MouseEvent) => e.preventDefault()}>${extra}<span>${label}</span></button>`;
  }
  private match(...s: string[]): boolean {
    const q = this.st.query.trim().toLowerCase();
    return !q || s.some((x) => x.toLowerCase().includes(q));
  }

  private devices() {
    const used = this.usedKeys();
    const devs = (this.cfg().devices ?? []).filter((d) => this.match(d.name ?? '', d.id, ...Object.keys(d.commands ?? {}), ...Object.keys(d.inputs ?? {})));
    if (!devs.length) return html`<p class="empty">${this.st.query ? 'Nothing matches.' : 'No devices yet. Import YAML or add a device first.'}</p>`;
    return devs.map((d) => {
      const g = deviceChips(d);
      const q = this.st.query.trim();
      const cards = GROUPS.map((grp) => {
        const chips = g[grp].filter((c) => !q || this.match(c.name, d.name ?? d.id, d.id));
        return chips.length
          ? html`<h4>${pretty(grp)}</h4><div class="chips" role="group" aria-label=${`${d.name ?? d.id} ${grp}`}>
              ${chips.map((c) => this.chip(c.name, { kind: c.kind, device: c.device, name: c.name }, used.has(c.kind === 'input' ? `${d.id}:input:${c.name}` : `${d.id}:${c.name}`), stockIconSvg(commandIconId(c.name, c.group))))}
            </div>`
          : nothing;
      });
      return html`<details open><summary>${stockIconSvg(1)}<span>${d.name ?? d.id}</span><small>${Object.keys(d.commands ?? {}).length} cmd</small></summary><div class="grp">${cards}</div></details>`;
    });
  }
  private activities() {
    const used = this.usedKeys();
    const acts = (this.cfg().activities ?? []).filter((a) => this.match(a.name ?? '', a.id));
    return html`
      <div class="chips">
        ${acts.map((a) => this.chip(a.name ?? a.id, { kind: 'activity', id: a.id }, used.has(`activity:${a.id}`), stockIconSvg(stockIconByName('play')?.id ?? 26, 'sic')))}
        ${this.match('all off', 'power off') ? this.chip('All off', { kind: 'alloff' }, used.has('activity:off'), stockIconSvg(stockIconByName('power')?.id ?? 7, 'sic')) : nothing}
      </div>
      ${acts.length ? nothing : html`<p class="empty">${this.st.query ? 'Nothing matches.' : 'No activities in this config.'}</p>`}
      <p class="hint">Drop one on a key to start it from that key, or on the screen to make a start button.</p>`;
  }
  override connectedCallback(): void {
    super.connectedCallback();
    this.poll = setInterval(() => {
      if (this.host?.state.tab === 'home' && !document.hidden) void this.host.state.loadHa(true);
    }, 10000);
  }
  private qTimer?: ReturnType<typeof setTimeout>;
  /** The palette re-renders hundreds of chips per query: wait for a pause in typing. */
  private search(q: string) {
    clearTimeout(this.qTimer);
    this.qTimer = setTimeout(() => this.st.set({ query: q }), 120);
  }
  private home() {
    const st = this.st;
    void st.loadHa();
    const ents = st.entities;
    const typedSource: Source | null = HA_ENTITY_ID.test(this.typed) ? { kind: 'entity', entity_id: this.typed } : null;
    const typed = html`<div class="typed">
      <input type="text" aria-label="Entity id" placeholder="light.living_room" .value=${this.typed} @input=${(e: Event) => (this.typed = (e.target as HTMLInputElement).value.trim())} />
      ${typedSource ? html`<div class="chips">${this.chip(this.typed, typedSource, false)}</div>` : html`<span class="hint">Type an entity id to get a chip you can drag.</span>`}
    </div>`;
    if (!st.haLoaded) return html`<p class="empty" role="status">Loading Home Assistant…</p>`;
    if (ents === null) return html`<p class="empty" role="status">Home Assistant cannot be read right now, so entities are not listed. You can still type an entity id.</p>${typed}`;
    const list = ents.filter((e) => HA_DOMAINS.includes(e.domain ?? entityDomain(e.entity_id)) && this.match(e.name ?? '', e.entity_id));
    const by = new Map<string, typeof list>();
    for (const e of list) by.set(e.domain ?? entityDomain(e.entity_id), [...(by.get(e.domain ?? entityDomain(e.entity_id)) ?? []), e]);
    const used = this.usedKeys();
    if (!list.length) return html`<p class="empty">${st.query ? 'Nothing matches.' : 'Home Assistant has no entities of the supported kinds.'}</p>${typed}`;
    return html`${[...by.entries()].map(
      ([dom, items]) => html`<details open><summary>${stockIconSvg(stockIconByName(DOMAIN_ICON[dom] ?? 'home')?.id ?? 13)}<span>${pretty(dom)}</span><small>${items.length}</small></summary>
        <div class="grp">${items.map((e) => {
          const src: Source = { kind: 'entity', entity_id: e.entity_id, name: e.name };
          return html`<button class="ent ${used.has(`ha:${e.entity_id}`) ? 'used' : ''}" aria-pressed=${this.pressed(src)} @pointerdown=${(ev: PointerEvent) => this.down(ev, src)} @keydown=${(ev: KeyboardEvent) => this.key(ev, src)} @click=${(ev: MouseEvent) => ev.preventDefault()}>
            ${stockIconSvg(stockIconByName(DOMAIN_ICON[dom] ?? 'home')?.id ?? 13)}<span class="t"><b>${e.name ?? e.entity_id}</b><i>${e.entity_id}</i></span>${e.state ? html`<span class="st ${e.state === 'on' ? 'on' : ''}">${e.state}</span>` : nothing}</button>`;
        })}</div></details>`,
    )}${typed}`;
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
          ${TABS.map(([id, label]) => html`<button role="tab" id=${`pt-${id}`} aria-selected=${st.tab === id} aria-controls="pbody" @click=${() => st.set({ tab: id })}>${label}</button>`)}
        </div>
        <input type="search" aria-label="Search the palette" placeholder="Search commands, activities, entities" .value=${st.query} @input=${(e: Event) => this.search((e.target as HTMLInputElement).value)} />
      </div>
      <div class="body" id="pbody" role="tabpanel" aria-labelledby=${`pt-${st.tab}`}>
        ${st.tab === 'devices' ? this.devices() : st.tab === 'activities' ? this.activities() : st.tab === 'home' ? this.home() : this.widgets()}
      </div>`;
  }
}
