import { LitElement, css, html, nothing, svg, type TemplateResult } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { repeat } from 'lit/directives/repeat.js';
import type { HaEntity } from '../lib/api';
import { stockIconSvg } from '../lib/icon-shapes';
import { stockIconByName } from '../lib/stock-icons';
import { chipStyles, shared } from '../lib/styles';
import {
  GROUP_BY, NONE, NO_AREA, NO_INTEGRATION, NO_LABEL, TYPE_CHIPS, availableGroupBys, domainOf, emptyFilters, facet, filterCount, filterEntities, groupEntities, groupIds, highlight,
  pushRecent, queryWords, readList, readText, reasonText, toggled, typeLabel, typeOf, writeList, noLookups, type Lookups, writeText, type Dim, type Filters, type Group, type GroupBy,
} from '../lib/ha-nav';
import type { Source } from '../lib/studio-model';

const ROWS_PER_GROUP = 30; // rows a leaf group shows before "Show more"
const MORE_STEP = 60;
const ROW_BUDGET = 300; // rows drawn across all groups, however many are open
const POP_MAX = 80;
const ICON: Record<string, string> = {
  light: 'bulb', switch: 'switch', scene: 'scene', script: 'play', media_player: 'tv', climate: 'thermo', cover: 'blinds', fan: 'fan', input: 'plus', sensor: 'info', lock: 'lock',
};
const GROUP_ICON: Record<string, string> = { floor: 'home', area: 'home', device: 'app', integration: 'settings', label: 'star' };
const DIMS: { dim: Dim; label: string; ic: string }[] = [
  { dim: 'types', label: 'Type', ic: 'list' },
  { dim: 'areas', label: 'Area', ic: 'home' },
  { dim: 'labels', label: 'Label', ic: 'star' },
  { dim: 'integrations', label: 'Integration', ic: 'settings' },
];
const icon = (name: string) => stockIconSvg(stockIconByName(name)?.id ?? 13);
const sourceOf = (e: HaEntity): Source => ({ kind: 'entity', entity_id: e.entity_id, name: e.name });
const ents = (n: number) => `${n.toLocaleString('en')} ${n === 1 ? 'entity' : 'entities'}`;
const PINS = [
  { id: 'fav', title: 'Favourites' },
  { id: 'used', title: 'Used in This Config' },
  { id: 'recent', title: 'Recently Used' },
] as const;

/**
 * Navigator for a big Home Assistant install: search, group-by, filters, collapsible groups, favourites.
 * One component for the palette (rows are dragged or picked), the action picker and the add-device
 * wizard (a row is picked with a click, `mode="select"`).
 */
@customElement('hh-ha-browser')
export class HhHaBrowser extends LitElement {
  static override styles = [
    shared,
    chipStyles,
    css`
      :host { display: grid; grid-template-columns: minmax(0, 1fr); gap: 8px; min-width: 0; container-type: inline-size; }
      .search { position: relative; }
      input[type="search"] { height: 34px; width: 100%; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 32px 0 10px; }
      input[type="search"]::-webkit-search-cancel-button { display: none; }
      .clr { position: absolute; right: 2px; top: 2px; width: 30px; height: 30px; border: 0; border-radius: var(--r1); background: transparent; color: var(--tx2); font-size: 18px; line-height: 1; cursor: pointer; }
      .clr:hover { background: var(--sf2); }
      .tools { display: flex; gap: 6px; align-items: center; flex-wrap: nowrap; height: 30px; }
      .tools label { display: flex; align-items: center; gap: 6px; color: var(--tx2); font-size: 12px; font-weight: 600; min-width: 0; white-space: nowrap; }
      .tools select { flex: none; }
      @container (max-width: 230px) { .tools .gb { display: none; } }
      .tools .btn, .tools .fchip { flex: none; }
      input[type="search"]::placeholder { text-overflow: ellipsis; }
      select { height: 28px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 6px; font-size: 12px; }
      .tools .sp { flex: 1; }
      .tools .btn { padding: 0 6px; }
      .tools .btn.icon { width: 30px; padding: 0; }
      .fchip.menu { font-size: 16px; line-height: 1; padding: 0 9px; }
      .fchips { display: flex; flex-wrap: nowrap; gap: 4px; min-width: 0; }
      .fchips .fchip { flex: none; }
      .fchip .car { flex: none; }
      .fchip .sic { width: 14px; height: 14px; flex: none; }
      .fchips.icons .l, .fchips.icons .car { display: none; }
      .fchips.icons .fchip { padding: 0 8px; }
      .fchip { display: inline-flex; align-items: center; gap: 4px; height: 26px; padding: 0 7px; border-radius: 13px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-size: 11.5px; font-weight: 600; white-space: nowrap; cursor: pointer; }
      .fchip:hover { background: var(--sf2); }
      .fchip small { color: var(--tx3); font-weight: 500; font-size: 11px; }
      .fchip[aria-pressed="true"], .fchip[aria-expanded="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .fchip .car { width: 6px; height: 6px; border-right: 1.5px solid currentColor; border-bottom: 1.5px solid currentColor; transform: translateY(-1px) rotate(45deg); }
      .pop { border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); padding: 8px; display: grid; grid-template-columns: minmax(0, 1fr); gap: 6px; }
      .pop input[type="search"] { height: 30px; padding-right: 10px; }
      .opts { display: grid; max-height: 200px; overflow: auto; }
      .opt { display: flex; align-items: center; gap: 8px; min-height: 30px; padding: 0 4px; font-size: 12.5px; cursor: pointer; border-radius: var(--r1); }
      .opt:hover { background: var(--sf2); }
      .opt span { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .opt small, .more, .sum, .note { color: var(--tx3); font-size: 11.5px; }
      .pills { display: flex; flex-wrap: wrap; gap: 4px; align-items: center; }
      .pill { display: inline-flex; align-items: center; gap: 4px; height: 24px; padding: 0 4px 0 9px; border-radius: 12px; border: 1px solid var(--acc); background: var(--accs); color: var(--tx); font-size: 11.5px; cursor: pointer; max-width: 100%; }
      .pill span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .pill b { font-size: 14px; line-height: 1; padding: 0 4px; color: var(--tx2); }
      .count { font-size: 11.5px; font-weight: 700; color: var(--tx); }
      .gh { display: flex; align-items: center; gap: 8px; width: 100%; min-height: 36px; padding: 0 8px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); color: var(--tx); font-weight: 650; font-size: 13px; text-align: left; cursor: pointer; }
      .gh.d1 { width: calc(100% - 10px); margin-left: 10px; }
      .gh.d2 { width: calc(100% - 20px); margin-left: 20px; }
      .gh.d3 { width: calc(100% - 30px); margin-left: 30px; }
      .gh:hover { background: var(--sf2); }
      .gh .sic { width: 18px; height: 18px; color: var(--acc); }
      .gh .gt { min-width: 0; flex: 0 1 auto; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .gh .gs { color: var(--tx3); font-weight: 500; font-size: 11px; flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .gh .cnt { margin-left: auto; color: var(--tx2); font-size: 11px; font-weight: 600; background: var(--sf2); border-radius: 9px; padding: 1px 7px; }
      .gh .gs + .cnt { margin-left: 0; }
      .gh.sub { min-height: 30px; font-size: 12px; border-style: dashed; }
      .chev { flex: none; width: 7px; height: 7px; border-right: 2px solid var(--tx2); border-bottom: 2px solid var(--tx2); transform: rotate(-45deg); transition: transform 100ms; margin: 0 2px; }
      .gh[aria-expanded="true"] .chev { transform: rotate(45deg); }
      .rows { display: grid; grid-template-columns: minmax(0, 1fr); gap: 4px; padding: 4px 0 6px 12px; }
      .row { display: flex; gap: 4px; align-items: stretch; content-visibility: auto; contain-intrinsic-size: auto 44px; }
      .ent.chip2 { height: 44px; border-radius: var(--r1); flex: 1; min-width: 0; padding: 0 8px; }
      .ent .sic { width: 18px; height: 18px; color: var(--tx2); }
      .ent[aria-pressed="true"] .sic { color: inherit; }
      .ent .t { min-width: 0; flex: 1; display: grid; line-height: 1.25; }
      .ent .t b, .ent .t i { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .ent .t b { font-weight: 550; font-size: 13px; }
      .ent .t i { font-style: normal; font-size: 11px; color: var(--tx3); }
      .ent[aria-pressed="true"] .t i { color: inherit; }
      mark { background: var(--accs); color: inherit; border-radius: 3px; padding: 0 1px; margin: 0 -1px; }
      .ent[aria-pressed="true"] mark { background: rgba(255, 255, 255, 0.28); }
      .star { flex: none; width: 34px; border: 1px solid transparent; border-radius: var(--r1); background: transparent; color: var(--tx3); font-size: 16px; cursor: pointer; }
      .star:hover { background: var(--sf2); color: var(--tx); }
      .star[aria-pressed="true"] { color: var(--acc); }
      .more { width: 100%; height: 30px; border: 1px dashed var(--line); border-radius: var(--r1); background: transparent; cursor: pointer; color: var(--tx2); }
      .more:hover { background: var(--sf2); }
      .err { display: grid; grid-template-columns: minmax(0, 1fr); gap: 8px; }
      .list { display: grid; grid-template-columns: minmax(0, 1fr); gap: 6px; }
      .skel { display: grid; grid-template-columns: minmax(0, 1fr); gap: 6px; }
      .skel i { display: block; height: 36px; border-radius: var(--r2); background: linear-gradient(90deg, var(--sf2), var(--sf3), var(--sf2)); background-size: 200% 100%; animation: sh 1.2s linear infinite; }
      @keyframes sh { to { background-position: -200% 0; } }
      @media (prefers-reduced-motion: reduce) { .skel i { animation: none; } .chev { transition: none; } }
      .empty { color: var(--tx2); font-size: 12.5px; padding: 8px 2px; display: grid; gap: 8px; justify-items: start; }
      .note { padding: 6px 8px; border-radius: var(--r1); background: var(--sf2); }
      .chk { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--tx2); }
      @media (max-width: 720px) { .fchip { height: 32px; } .gh { min-height: 44px; } .ent.chip2 { height: 52px; } .star { width: 44px; } select, .opt { min-height: 36px; } }
    `,
  ];

  @property({ attribute: false }) entities: HaEntity[] | null = null;
  @property({ type: Boolean }) loaded = false;
  @property({ type: Boolean }) loading = false;
  @property() reason = '';
  @property({ type: Boolean }) registries = false;
  @property({ attribute: false }) lookups: Lookups = noLookups;
  /** the number of matching entities when the add-on cut its list at the cap (else 0) */
  @property({ type: Number }) truncated = 0;
  /** only these domains are listed (the add-device step lists a few) */
  @property({ attribute: false }) domains: readonly string[] | null = null;
  /** entity id -> services the draft already calls on it */
  @property({ attribute: false }) used: ReadonlyMap<string, string[]> = new Map();
  /** the entity picked up (palette click-to-place) */
  @property() pressed = '';
  /** drag: palette rows are dragged or picked; select: a click picks */
  @property() mode: 'drag' | 'select' = 'drag';
  @property({ type: Boolean }) diag = false;
  /** namespace of the remembered group-by and open groups */
  @property() scope = 'palette';
  @property() label = 'Home Assistant entities';

  @state() private f: Filters = emptyFilters();
  @state() private draft = '';
  @state() private by: GroupBy | '' = '';
  @state() private open = new Set<string>();
  @state() private searchClosed = new Set<string>();
  @state() private limits = new Map<string, number>();
  @state() private pop: Dim | 'menu' | null = null;
  @state() private popQ = '';
  @state() private favs: string[] = readList('fav');
  @state() private recent: string[] = readList('recent');
  @state() private pinOpen = new Set<string>(readList('pinopen'));

  private base: HaEntity[] = [];
  private byId = new Map<string, HaEntity>();
  private bys: GroupBy[] = ['type'];
  private qTimer?: ReturnType<typeof setTimeout>;
  private memo: { key: string; list: HaEntity[]; groups: Group[]; facets: Map<Dim, Map<string, number>> } | null = null;
  private memoSrc: unknown;

  private ro?: ResizeObserver;
  /** the filter buttons show icon + word when all four fit on the line, else the same four as icons alone (the tooltip and aria-label keep the word) */
  private fitChips = () => {
    const row = this.shadowRoot?.querySelector<HTMLElement>('.fchips');
    if (!row) return;
    row.classList.remove('icons');
    row.classList.toggle('icons', row.scrollWidth > row.clientWidth + 0.5);
  };
  protected override updated(): void {
    this.fitChips();
    if (!this.ro) {
      this.ro = new ResizeObserver(this.fitChips);
      this.ro.observe(this);
    }
  }
  override connectedCallback(): void {
    super.connectedCallback();
    window.addEventListener('keydown', this.slash);
    window.addEventListener('click', this.outside);
  }
  override disconnectedCallback(): void {
    this.ro?.disconnect();
    this.ro = undefined;
    window.removeEventListener('keydown', this.slash);
    window.removeEventListener('click', this.outside);
    clearTimeout(this.qTimer);
    super.disconnectedCallback();
  }
  /** `/` focuses the search unless the user is typing somewhere else */
  /** a click outside an open filter list, or Escape in it, closes it */
  private outside = (e: Event) => {
    if (this.pop && !e.composedPath().some((n) => n instanceof HTMLElement && (n.classList.contains('pop') || n.hasAttribute('aria-haspopup')))) this.pop = null;
  };
  private slash = (e: KeyboardEvent) => {
    if (e.key === 'Escape' && this.pop) {
      this.pop = null;
      return;
    }
    if (e.key !== '/' || e.ctrlKey || e.metaKey || e.altKey || !this.getClientRects().length) return;
    const t = e.composedPath()[0] as HTMLElement | undefined;
    if (t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable)) return;
    e.preventDefault();
    this.shadowRoot?.querySelector<HTMLInputElement>('input.q')?.focus();
  };

  protected override willUpdate(ch: Map<string, unknown>): void {
    if (ch.has('entities') || ch.has('domains') || ch.has('lookups') || ch.has('registries')) {
      const all = this.entities ?? [];
      const dom = this.domains;
      this.base = dom ? all.filter((e) => dom.includes(domainOf(e))) : all;
      this.byId = new Map(all.map((e) => [e.entity_id, e]));
      this.bys = availableGroupBys(this.base, this.registries);
      this.memo = null;
    }
    if (!this.by || !this.bys.includes(this.by)) {
      const saved = readText(`by.${this.scope}`, 'area') as GroupBy;
      this.by = this.bys.includes(saved) ? saved : (this.bys[0] as GroupBy);
      this.open = new Set(readList(`open.${this.scope}.${this.by}`));
    }
  }

  // -------------------------------------------------------------- derived view
  private view(): { list: HaEntity[]; groups: Group[]; facets: Map<Dim, Map<string, number>> } {
    const f = this.f;
    const key = [f.q, [...f.types], [...f.areas], [...f.labels], [...f.integrations], this.by].join('|');
    if (!this.memo || this.memo.key !== key || this.memoSrc !== this.base) {
      const list = filterEntities(this.base, f);
      this.memo = { key, list, facets: new Map(), groups: groupEntities(list, this.by as GroupBy, this.lookups) };
      this.memoSrc = this.base;
    }
    return this.memo;
  }
  /** facet counts for the open filter list, computed once per search and filter state */
  private facetOf(dim: Dim): Map<string, number> {
    const m = this.view().facets;
    let c = m.get(dim);
    if (!c) m.set(dim, (c = facet(this.base, this.f, dim)));
    return c;
  }
  private get searching(): boolean {
    return this.f.q.trim() !== '';
  }
  private isOpen(id: string): boolean {
    return this.searching ? !this.searchClosed.has(id) : this.open.has(id);
  }
  private toggle(id: string) {
    const set = new Set(this.searching ? this.searchClosed : this.open);
    const closing = this.isOpen(id);
    if (this.searching) {
      if (closing) set.add(id);
      else set.delete(id);
      this.searchClosed = set;
    } else {
      if (closing) set.delete(id);
      else set.add(id);
      this.open = set;
      writeList(`open.${this.scope}.${this.by}`, [...set]);
    }
  }
  private setAll(expand: boolean) {
    const ids = groupIds(this.view().groups);
    if (this.searching) this.searchClosed = expand ? new Set() : new Set(ids);
    else {
      this.open = expand ? new Set(ids) : new Set();
      writeList(`open.${this.scope}.${this.by}`, [...this.open]);
    }
  }
  private setBy(by: GroupBy) {
    this.by = by;
    writeText(`by.${this.scope}`, by);
    this.open = new Set(readList(`open.${this.scope}.${by}`));
    this.searchClosed = new Set();
  }
  private setQ(q: string, now = false) {
    clearTimeout(this.qTimer);
    const apply = () => {
      this.f = { ...this.f, q };
      this.searchClosed = new Set();
    };
    if (now) apply();
    else this.qTimer = setTimeout(apply, 120);
  }
  private setSet(dim: Dim, v: ReadonlySet<string>) {
    this.f = { ...this.f, [dim]: v };
  }
  private flip(dim: Dim, k: string) {
    const s = new Set(this.f[dim]);
    if (!s.delete(k)) s.add(k);
    this.setSet(dim, s);
  }
  private clearFilters() {
    this.f = { ...emptyFilters(), q: this.f.q };
    this.pop = null;
  }

  // -------------------------------------------------------------- picking
  private remember(e: HaEntity) {
    this.recent = pushRecent(this.recent, e.entity_id);
    writeList('recent', this.recent);
  }
  private down(ev: PointerEvent, e: HaEntity) {
    if (this.mode !== 'drag' || ev.button !== 0) return;
    this.remember(e);
    this.dispatchEvent(new CustomEvent('hh-chip-down', { detail: { source: sourceOf(e), ev }, bubbles: true, composed: true }));
  }
  private pick(ev: MouseEvent, e: HaEntity) {
    ev.preventDefault();
    if (this.mode !== 'select') return;
    this.remember(e);
    this.dispatchEvent(new CustomEvent('hh-ha-pick', { detail: e, bubbles: true, composed: true }));
  }
  private rowKey(ev: KeyboardEvent, e: HaEntity) {
    if (this.mode !== 'drag' || (ev.key !== ' ' && ev.key !== 'Enter')) return;
    ev.preventDefault();
    ev.stopPropagation(); // the studio's window handler would read this Enter as "drop"
    this.remember(e);
    this.dispatchEvent(new CustomEvent('hh-chip-pick', { detail: { source: sourceOf(e) }, bubbles: true, composed: true }));
  }
  private star(id: string) {
    this.favs = toggled(this.favs, id);
    writeList('fav', this.favs);
  }

  /** arrows move through headers and rows; Left/Right collapse and expand a header */
  private nav(ev: KeyboardEvent) {
    const t = ev.target as HTMLElement;
    if (!t.matches?.('[data-nav]')) return;
    const all = [...(this.shadowRoot?.querySelectorAll<HTMLElement>('[data-nav]') ?? [])];
    const i = all.indexOf(t);
    let to = -1;
    if (ev.key === 'ArrowDown') to = i + 1;
    else if (ev.key === 'ArrowUp') to = i - 1;
    else if (ev.key === 'Home') to = 0;
    else if (ev.key === 'End') to = all.length - 1;
    else if ((ev.key === 'ArrowRight' || ev.key === 'ArrowLeft') && t.hasAttribute('aria-expanded')) {
      const want = ev.key === 'ArrowRight';
      if ((t.getAttribute('aria-expanded') === 'true') !== want) t.click();
      ev.preventDefault();
      ev.stopPropagation();
      return;
    }
    if (to < 0) return;
    ev.preventDefault();
    ev.stopPropagation();
    all[Math.max(0, Math.min(all.length - 1, to))]?.focus();
  }

  // -------------------------------------------------------------- rendering
  private hl(text: string, words: readonly string[]) {
    return highlight(text, words).map((p) => (p.m ? html`<mark>${p.t}</mark>` : p.t));
  }
  private row(e: HaEntity, words: readonly string[], extra = ''): TemplateResult {
    const used = this.used.has(e.entity_id);
    const sec = [e.entity_id, this.by !== 'area' && e.area, this.by !== 'device' && e.device, extra].filter(Boolean).join(' · ');
    const tip = [e.name ?? e.entity_id, e.entity_id, e.area, e.device, e.platform, used ? 'already used in this config' : ''].filter(Boolean).join(' · ');
    const fav = this.favs.includes(e.entity_id);
    const ic = ICON[typeOf(e)] ?? 'home';
    return html`<div class="row">
      <button class="ent chip2 ${used ? 'used' : ''}" data-nav data-id=${e.entity_id} aria-pressed=${this.mode === 'drag' ? String(this.pressed === e.entity_id) : nothing} title=${tip}
        @pointerdown=${(ev: PointerEvent) => this.down(ev, e)} @keydown=${(ev: KeyboardEvent) => this.rowKey(ev, e)} @click=${(ev: MouseEvent) => this.pick(ev, e)}>
        ${icon(ic)}<span class="t"><b>${this.hl(e.name ?? e.entity_id, words)}</b><i>${this.hl(sec, words)}</i></span>
      </button>
      <button class="star" aria-pressed=${fav} aria-label=${`${fav ? 'Remove' : 'Add'} ${e.name ?? e.entity_id} ${fav ? 'from' : 'to'} favourites`} title=${fav ? 'Remove from favourites' : 'Add to favourites'} @click=${() => this.star(e.entity_id)}>${fav ? '★' : '☆'}</button>
    </div>`;
  }
  private rowsOf(id: string, title: string, items: readonly HaEntity[], budget: { n: number }, words: readonly string[], extra?: (e: HaEntity) => string) {
    const lim = this.limits.get(id) ?? ROWS_PER_GROUP;
    const take = Math.min(items.length, lim > ROWS_PER_GROUP ? lim : Math.min(lim, Math.max(0, budget.n)));
    budget.n -= Math.min(take, ROWS_PER_GROUP); // rows added with "Show more" are on top of the budget, so later groups do not move
    const rest = items.length - take;
    return html`<div class="rows" role="group" aria-label=${title}>
      ${repeat(items.slice(0, take), (e) => e.entity_id, (e) => this.row(e, words, extra?.(e) ?? ''))}
      ${rest > 0 ? html`<button class="more" data-nav @click=${() => (this.limits = new Map(this.limits).set(id, take + MORE_STEP))}>Show ${Math.min(rest, MORE_STEP).toLocaleString('en')} more (${rest.toLocaleString('en')} not shown)</button>` : nothing}
    </div>`;
  }
  private header(id: string, title: string, count: number, open: boolean, ic: TemplateResult | typeof nothing, words: readonly string[], sub = '', depth = 0, onToggle?: () => void) {
    return html`<button class="gh d${Math.min(depth, 3)} ${depth > 1 ? 'sub' : ''}" data-nav aria-expanded=${open} @click=${onToggle ?? (() => this.toggle(id))}>
      <i class="chev" aria-hidden="true"></i>${ic}<span class="gt" title=${title}>${this.hl(title, words)}</span>${sub ? html`<span class="gs">${sub}</span>` : nothing}<span class="cnt">${count.toLocaleString('en')}</span>
    </button>`;
  }
  private group(g: Group, depth: number, budget: { n: number }, words: readonly string[]): TemplateResult {
    const open = this.isOpen(g.id);
    const ic = g.kind === 'type' ? icon(ICON[g.type ?? ''] ?? 'home') : icon(GROUP_ICON[g.kind] ?? 'home');
    return html`<div class="grp">
      ${this.header(g.id, g.title, g.count, open, depth === 0 || g.kind === 'area' || g.kind === 'floor' ? ic : nothing, words, g.sub && depth === 0 ? g.sub : '', depth)}
      ${open ? (g.children ? repeat(g.children, (c) => c.id, (c) => this.group(c, depth + 1, budget, words)) : this.rowsOf(g.id, g.title, g.items ?? [], budget, words)) : nothing}
    </div>`;
  }
  private pinned(words: readonly string[], budget: { n: number }) {
    const q = this.f.q.trim() !== '';
    const keep = (e: HaEntity) => !words.length || filterEntities([e], { ...emptyFilters(), q: this.f.q }).length > 0;
    const get = (id: string): HaEntity | undefined => this.byId.get(id) ?? (this.used.has(id) ? { entity_id: id, name: id } : undefined);
    const lists: Record<(typeof PINS)[number]['id'], HaEntity[]> = {
      fav: this.favs.map(get).filter((e): e is HaEntity => !!e),
      used: [...this.used.keys()].sort().map(get).filter((e): e is HaEntity => !!e),
      recent: this.recent.map(get).filter((e): e is HaEntity => !!e),
    };
    return PINS.map(({ id, title }) => {
      const items = lists[id].filter(keep);
      if (!items.length || (q && id === 'recent')) return nothing; // a search result list does not need the recents repeated
      const open = q || this.pinOpen.has(id); // a search opens the pinned lists that match
      const gid = `pin:${id}`;
      return html`<div class="grp">
        ${this.header(gid, title, items.length, open, icon(id === 'fav' ? 'star' : id === 'used' ? 'check' : 'play'), [], '', 0, () => {
          const s = new Set(this.pinOpen);
          if (this.pinOpen.has(id)) s.delete(id);
          else s.add(id);
          this.pinOpen = s;
          writeList('pinopen', [...s]);
        })}
        ${open ? this.rowsOf(gid, title, items, budget, q ? words : [], id === 'used' ? (e) => (this.used.get(e.entity_id) ?? []).join(', ') : undefined) : nothing}
      </div>`;
    });
  }
  private dropdown(dim: Dim, label: string, ic: string) {
    const has: Record<string, GroupBy | ''> = { types: '', areas: 'area', labels: 'label', integrations: 'integration' };
    const n = this.f[dim].size;
    const g = has[dim] as GroupBy | '';
    if (g && !this.bys.includes(g) && !n) return nothing; // no data to filter by
    return html`<button class="fchip" aria-expanded=${this.pop === dim} aria-haspopup="true" aria-label=${n ? `${label} ${n}` : label} title=${label} @click=${() => { this.pop = this.pop === dim ? null : dim; this.popQ = ''; }}>${icon(ic)}<span class="l">${label}</span>${n ? html`<small>${n}</small>` : nothing}<i class="car" aria-hidden="true"></i></button>`;
  }
  private menu() {
    return html`<button class="fchip menu" aria-label="Options" title="Options" aria-expanded=${this.pop === 'menu'} aria-haspopup="true" @click=${() => (this.pop = this.pop === 'menu' ? null : 'menu')}>⋯</button>`;
  }
  /** the display name of a filter key (ids are keys, names are for people) */
  private nameOf(dim: Dim, k: string): string {
    if (dim === 'types') return typeLabel(k);
    if (k === NONE) return dim === 'areas' ? NO_AREA : dim === 'labels' ? NO_LABEL : NO_INTEGRATION;
    return dim === 'areas' ? (this.lookups.areas.get(k)?.label ?? k) : dim === 'labels' ? (this.lookups.labels.get(k) ?? k) : k;
  }
  private popPanel() {
    const dim = this.pop;
    if (!dim) return nothing;
    if (dim === 'menu')
      return html`<div class="pop" role="group" aria-label="Options">
        ${this.registries && this.mode === 'drag' ? html`<label class="chk"><input type="checkbox" .checked=${this.diag} @change=${(e: Event) => this.dispatchEvent(new CustomEvent('hh-ha-diag', { detail: (e.target as HTMLInputElement).checked, bubbles: true, composed: true }))} /> Show Diagnostic and Config Entities</label>` : nothing}
        <button class="btn sm" @click=${() => this.dispatchEvent(new CustomEvent('hh-ha-refresh', { bubbles: true, composed: true }))}>Refresh the List</button>
      </div>`;
    const none = dim === 'types' ? '' : NONE;
    const counts = this.facetOf(dim);
    const sel = this.f[dim];
    const words = queryWords(this.popQ);
    const name = (k: string) => this.nameOf(dim, k);
    const order = (k: string) => TYPE_CHIPS.findIndex((t) => t.id === k);
    const names = [...new Set([...counts.keys(), ...sel])].sort((a, b) =>
      dim === 'types' ? order(a) - order(b) : a === none ? 1 : b === none ? -1 : Number(sel.has(b)) - Number(sel.has(a)) || name(a).localeCompare(name(b), undefined, { sensitivity: 'base', numeric: true }),
    );
    const shown = names.filter((n) => !words.length || words.every((w) => name(n).toLowerCase().includes(w)));
    const label = DIMS.find((d) => d.dim === dim)?.label ?? '';
    return html`<div class="pop" role="group" aria-label=${`${label} filter`}>
      ${names.length > 8 ? html`<input type="search" aria-label=${`Search ${label.toLowerCase()}s`} placeholder=${`Search ${label.toLowerCase()}s`} .value=${this.popQ} @input=${(e: Event) => (this.popQ = (e.target as HTMLInputElement).value)} />` : nothing}
      <div class="opts">
        ${shown.slice(0, POP_MAX).map((n) => html`<label class="opt"><input type="checkbox" .checked=${sel.has(n)} @change=${() => this.flip(dim, n)} />${dim === 'types' ? icon(ICON[n] ?? 'home') : nothing}<span>${name(n)}</span><small>${(counts.get(n) ?? 0).toLocaleString('en')}</small></label>`)}
        ${shown.length > POP_MAX ? html`<span class="more">${shown.length - POP_MAX} more: type to narrow</span>` : nothing}
        ${shown.length ? nothing : html`<span class="more">Nothing matches</span>`}
      </div>
    </div>`;
  }
  private pills() {
    const out: TemplateResult[] = [];
    const add = (dim: Dim, k: string, text: string) =>
      out.push(html`<button class="pill" aria-label=${`Remove filter ${text}`} @click=${() => this.flip(dim, k)}><span>${text}</span><b aria-hidden="true">×</b></button>`);
    this.f.types.forEach((k) => add('types', k, this.nameOf('types', k)));
    this.f.areas.forEach((k) => add('areas', k, `Area: ${this.nameOf('areas', k)}`));
    this.f.labels.forEach((k) => add('labels', k, `Label: ${this.nameOf('labels', k)}`));
    this.f.integrations.forEach((k) => add('integrations', k, `Integration: ${this.nameOf('integrations', k)}`));
    return out;
  }

  protected override render() {
    if (this.entities === null && this.loaded && !this.loading)
      return html`<div class="banner bad err" role="alert"><b>Home Assistant cannot be read</b><span>${reasonText(this.reason)}</span><span><button class="btn sm" @click=${() => this.dispatchEvent(new CustomEvent('hh-ha-retry', { bubbles: true, composed: true }))}>Retry</button></span></div>`;
    if (this.entities === null)
      return html`<div class="skel" role="status" aria-label="Loading Home Assistant">${[0, 1, 2, 3, 4, 5].map(() => html`<i></i>`)}</div>`;
    const { list, groups } = this.view();
    const words = queryWords(this.f.q);
    const nf = filterCount(this.f);
    const total = this.base.length;
    const budget = { n: ROW_BUDGET };
    const anyOpen = this.searching ? groups.length > 0 && this.searchClosed.size < groupIds(groups).length : this.open.size > 0;
    const pinned = this.pinned(words, budget);
    const summary = list.length === total ? ents(total) : `${list.length.toLocaleString('en')} of ${ents(total)}`;
    return html`
      <div class="search">
        <input class="q" type="search" aria-label=${`Search ${this.label}`} placeholder="Search entities, areas, devices ( / )" .value=${this.draft} autocomplete="off"
          @input=${(e: Event) => { this.draft = (e.target as HTMLInputElement).value; this.setQ(this.draft); }}
          @keydown=${(e: KeyboardEvent) => { if (e.key === 'Escape' && this.draft) { e.stopPropagation(); this.draft = ''; this.setQ('', true); } }} />
        ${this.draft ? html`<button class="clr" aria-label="Clear search" @click=${() => { this.draft = ''; this.setQ('', true); this.shadowRoot?.querySelector<HTMLInputElement>('input.q')?.focus(); }}>×</button>` : nothing}
      </div>
      <div class="fchips" role="group" aria-label="Filters">
        ${DIMS.map((d) => this.dropdown(d.dim, d.label, d.ic))}
      </div>
      ${this.pop === 'menu' ? nothing : this.popPanel()}
      ${nf ? html`<div class="pills"><span class="count">${nf} ${nf === 1 ? 'filter' : 'filters'}</span>${this.pills()}<button class="btn ghost sm" @click=${() => this.clearFilters()}>Clear</button></div>` : nothing}
      <div class="tools">
        <label><span class="gb">Group By</span>
          <select aria-label="Group by" .value=${this.by} @change=${(e: Event) => this.setBy((e.target as HTMLSelectElement).value as GroupBy)}>
            ${GROUP_BY.filter((g) => this.bys.includes(g.id)).map((g) => html`<option value=${g.id} ?selected=${g.id === this.by}>${g.label}</option>`)}
          </select>
        </label>
        <span class="sp"></span>
        <button class="btn ghost sm icon" aria-label=${anyOpen ? 'Collapse All' : 'Expand All'} title=${anyOpen ? 'Collapse All' : 'Expand All'} @click=${() => this.setAll(!anyOpen)}>
          <svg class="ic" viewBox="0 0 24 24" aria-hidden="true">${anyOpen ? svg`<path d="M6 15l6-6 6 6M6 21l6-6 6 6" transform="translate(0 -6)"/>` : svg`<path d="M6 9l6 6 6-6M6 3l6 6 6-6" transform="translate(0 6)"/>`}</svg>
        </button>
        ${this.menu()}
      </div>
      ${this.pop === 'menu' ? this.popPanel() : nothing}
      ${this.registries ? nothing : html`<p class="note" role="note">Areas, devices, labels and integrations could not be read from Home Assistant, so entities are grouped by type only.</p>`}
      ${this.truncated ? html`<p class="note" role="note">Showing the first ${list.length.toLocaleString('en')} of ${this.truncated.toLocaleString('en')} entities. Narrow it with the search or the filters.</p>` : nothing}
      <div class="sum" role="status" aria-live="polite">${summary}${this.searching ? ` match “${this.f.q.trim()}”` : nf ? ' match the filters' : ''}</div>
      <div class="list" @keydown=${this.nav}>
        ${pinned}
        ${list.length ? repeat(groups, (g) => g.id, (g) => this.group(g, 0, budget, words)) : html`<div class="empty">${total ? html`<span>No entities match${this.searching ? ` “${this.f.q.trim()}”` : ''}${nf ? ' with these filters' : ''}.</span><button class="btn sm" @click=${() => { this.draft = ''; this.setQ('', true); this.clearFilters(); }}>Clear Filters</button>` : html`<span>Home Assistant has no entities of the supported kinds.</span>`}</div>`}
      </div>`;
  }
}
