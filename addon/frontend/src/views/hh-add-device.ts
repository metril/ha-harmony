import { css, html, nothing } from 'lit';
import { customElement, query, state } from 'lit/decorators.js';
import '../components/hh-send-ir';
import { StoreElement } from '../components/base';
import type { HhSendIr } from '../components/hh-send-ir';
import { ApiError, type IrBrand, type IrDeviceResult, type IrModels, type IrParsed } from '../lib/api';
import { KIND_LABEL, KIND_TYPE, OTHER_TYPES, art, commandIconId, type Kind } from '../lib/device-art';
import '../components/hh-icon-picker';
import { capProblem, deviceIdFor, nameProblem } from '../lib/device-edit';
import { proposeFromEntity, PROPOSAL_DOMAINS } from '../lib/ha-propose';
import { icon } from '../lib/icons';
import { stockIconSvg } from '../lib/icon-shapes';
import type { Device, IrAction } from '../lib/model';
import { hashFor } from '../lib/router';
import { summarize, trimBytes } from '../lib/codes';
import { shared } from '../lib/styles';
import { StudioState } from '../lib/studio-state';
import { GROUPS, groupCommand, pretty, type Group } from '../lib/studio-model';
import { candidateLabel, currentCandidate, initial, isOffline, likelyModels, progressIndex, reduce, stepsFor, type WAction, type WState } from '../lib/wizard';

const RECENT = 'hh-recent-brands';
const readRecents = (): IrBrand[] => {
  try {
    return JSON.parse(localStorage.getItem(RECENT) ?? '[]') as IrBrand[];
  } catch {
    return [];
  }
};
const KINDS = Object.keys(KIND_LABEL) as Kind[];
const PAGE = 40;

/** Add-device wizard: a full-page stepper (source, brand, type, power test, model, review). Writes the draft only. */
@customElement('hh-add-device')
export class HhAddDevice extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 18px; min-width: 0; max-width: 1040px; width: 100%; margin: 0 auto; }
      .top { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
      .steps { display: flex; gap: 6px; align-items: center; flex: 1; justify-content: center; flex-wrap: wrap; }
      .steps li { list-style: none; display: flex; align-items: center; gap: 6px; color: var(--tx3); font-size: 12px; font-weight: 600; }
      .steps li::before { content: ''; width: 10px; height: 10px; border-radius: 50%; background: var(--sf3); }
      .steps li.done::before { background: var(--acc); }
      .steps li.now { color: var(--tx); } .steps li.now::before { background: var(--acc); box-shadow: 0 0 0 4px var(--accs); }
      .steps li + li::after { content: ''; }
      ol.steps { margin: 0; padding: 0; }
      h1 { font-size: 26px; text-align: center; margin: 4px 0 0; }
      .lead { text-align: center; color: var(--tx2); margin: -10px 0 0; }
      .tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr)); gap: 14px; }
      .tile { display: grid; gap: 8px; justify-items: center; text-align: center; padding: 18px 14px; min-height: 150px; align-content: center; border: 1.5px solid var(--line); border-radius: var(--r3); background: var(--sf); color: var(--tx); cursor: pointer; font-weight: 650; font-size: 16px; }
      .tile:hover:not(:disabled) { border-color: var(--acc); background: var(--accs); }
      .tile:disabled { opacity: 0.55; cursor: not-allowed; }
      .tile .art { width: 96px; height: 72px; color: var(--acc); }
      .tile small { font-weight: 400; color: var(--tx3); font-size: 12.5px; }
      a.tile { text-decoration: none; }
      .tile .big { width: 96px; height: 72px; padding: 8px; box-sizing: border-box; color: var(--acc); fill: currentColor; }
      .search { height: 52px; font-size: 18px; border: 1.5px solid var(--line); border-radius: var(--r3); background: var(--sf); color: var(--tx); padding: 0 18px; width: 100%; }
      .brands { display: grid; grid-template-columns: repeat(auto-fill, minmax(170px, 1fr)); gap: 10px; }
      .brand { display: grid; gap: 2px; padding: 14px; text-align: left; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); color: var(--tx); cursor: pointer; min-height: 72px; }
      .brand:hover, .brand:focus-visible { border-color: var(--acc); background: var(--accs); }
      .brand b { font-size: 18px; font-weight: 650; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .brand span { color: var(--tx3); font-size: 12px; }
      h3 { margin: 6px 0 0; font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); }
      .stage { display: grid; gap: 18px; justify-items: center; text-align: center; padding: 8px 0; }
      .q { font-size: 20px; font-weight: 650; }
      .answers { display: flex; gap: 10px; flex-wrap: wrap; justify-content: center; }
      .answers .btn { height: 48px; padding: 0 22px; font-size: 15px; }
      .btn.pri kbd { color: inherit; border-color: currentColor; opacity: 0.85; }
      kbd { font: inherit; font-size: 11px; border: 1px solid var(--line); border-bottom-width: 2px; border-radius: 4px; padding: 0 5px; margin-left: 6px; color: var(--tx3); }
      .tried { display: flex; gap: 6px; flex-wrap: wrap; justify-content: center; }
      .tried .chip { min-width: 28px; justify-content: center; }
      .models { display: grid; gap: 6px; }
      .model { display: flex; gap: 10px; align-items: center; justify-content: space-between; text-align: left; padding: 0 14px; min-height: 48px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); color: var(--tx); cursor: pointer; }
      .model:hover:not(:disabled) { border-color: var(--acc); background: var(--accs); }
      .pager { display: flex; gap: 10px; align-items: center; justify-content: center; }
      .likely { display: flex; gap: 8px; flex-wrap: wrap; }
      .likely .btn { height: 40px; }
      .review { display: grid; grid-template-columns: minmax(0, 1fr) 280px; gap: 16px; align-items: start; }
      .grp { margin-bottom: 14px; }
      .grp h3 { display: flex; gap: 8px; align-items: center; margin: 0 0 6px; }
      .grp h3 .sic { width: 16px; height: 16px; color: var(--acc); }
      .cmds { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 6px; }
      .cmd { display: flex; gap: 6px; align-items: center; padding: 0 6px 0 10px; min-height: 44px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); min-width: 0; }
      .cmd.off { opacity: 0.55; }
      .cmd input[type="text"] { flex: 1; min-width: 0; height: 30px; border: 1px solid transparent; background: transparent; color: var(--tx); font-weight: 600; border-radius: 6px; padding: 0 4px; }
      .cmd input[type="text"]:hover, .cmd input[type="text"]:focus { border-color: var(--line); background: var(--bg); }
      .cmd small { display: block; color: var(--tx3); font-family: var(--mono); font-size: 11px; padding: 0 4px; }
      .cmd .m { flex: 1; min-width: 0; }
      .side { display: grid; gap: 12px; position: sticky; top: 64px; }
      input[type="text"].namein { height: 38px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; width: 100%; }
      textarea { width: 100%; height: 150px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 8px 10px; font-family: var(--mono); font-size: 12px; }
      select { height: 36px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; }
      ul.plain { margin: 0; padding-left: 18px; font-size: 13px; color: var(--tx2); }
      .why { color: var(--tx3); font-size: 12.5px; margin: 0; }
      .err { color: var(--bad); font-size: 12px; }
      .ents { display: grid; gap: 6px; max-height: 52vh; overflow: auto; }
      .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
      .center { display: flex; justify-content: center; }
      @media (max-width: 860px) { .review { grid-template-columns: minmax(0, 1fr); } .side { position: static; } h1 { font-size: 22px; } .tile { min-height: 120px; } .tile .art { width: 72px; height: 54px; } }
    `,
  ];
  @state() private w: WState = initial();
  @state() private q = '';
  @state() private brands: IrBrand[] = [];
  @state() private brandsErr: ApiError | null = null;
  @state() private models: IrModels | null = null;
  @state() private offset = 0;
  @state() private mq = '';
  @state() private names: Record<string, string> = {};
  @state() private devName = '';
  @state() private devIcon = '';
  @state() private notes: string[] = [];
  @state() private text = '';
  @state() private kind: 'pronto' | 'flipper' | 'smartir' = 'pronto';
  @state() private parsed: IrParsed | null = null;
  @state() private parseErr = '';
  @state() private otherType = '14';
  @state() private choosingOther = false;
  @query('hh-send-ir') private sender?: HhSendIr;
  private ha = new StudioState(this.store);
  private timer?: ReturnType<typeof setTimeout>;
  private seq = 0;

  override connectedCallback(): void {
    super.connectedCallback();
    this.ha.addEventListener('change', this.onHa);
    void this.ha.loadCapabilities();
    window.addEventListener('keydown', this.onKey);
  }
  override disconnectedCallback(): void {
    this.ha.removeEventListener('change', this.onHa);
    window.removeEventListener('keydown', this.onKey);
    clearTimeout(this.timer);
    super.disconnectedCallback();
  }
  private onHa = () => this.requestUpdate();

  private dispatch(a: WAction) {
    this.w = reduce(this.w, a);
  }
  private get api() {
    return this.store.api;
  }
  private fail(scope: 'brands' | 'candidates' | 'models' | 'device', e: unknown) {
    const err = e instanceof ApiError ? e : new ApiError(0, 'network', 'Cannot reach the add-on.');
    this.dispatch({ t: 'fail', scope, code: err.code, msg: err.message });
  }

  // ------------------------------------------------------------ data loading, one per step
  private async loadBrands(q: string) {
    const n = ++this.seq;
    try {
      const r = await this.api.irBrands(q);
      if (n !== this.seq) return;
      this.brands = r.brands;
      this.brandsErr = null;
    } catch (e) {
      if (n !== this.seq) return;
      this.brandsErr = e instanceof ApiError ? e : new ApiError(0, 'network', 'Cannot reach the add-on.');
    }
  }
  private async loadCandidates() {
    const { brand, type } = this.w;
    if (!brand || !type) return;
    try {
      const r = await this.api.irCandidates(brand.slug, type);
      this.dispatch({ t: 'candidates', candidates: r.candidates, truncated: r.truncated });
      if (this.w.step === 'model') void this.loadModels(0, '');
    } catch (e) {
      this.fail('candidates', e);
      if (this.w.step === 'model') void this.loadModels(0, '');
    }
  }
  private async loadModels(offset: number, q: string) {
    const brand = this.w.brand;
    if (!brand) return;
    this.offset = offset;
    try {
      this.models = await this.api.irModels(brand.slug, q, offset, PAGE);
    } catch (e) {
      this.models = null;
      this.fail('models', e);
    }
  }
  private async pickModel(file: string, name: string) {
    const brand = this.w.brand;
    if (!brand) return;
    this.dispatch({ t: 'model', model: { file, name, id: 0 } });
    try {
      const r = await this.api.irDevice(brand.slug, file);
      this.names = {};
      this.devName = trimBytes(r.device.name ?? r.device.id);
      this.notes = r.notes;
      this.dispatch({ t: 'device', result: r });
    } catch (e) {
      this.fail('device', e);
    }
  }
  /** A "likely model" is only a name: find its file through the model search. */
  private async pickLikely(name: string) {
    const brand = this.w.brand;
    if (!brand) return;
    try {
      const r = await this.api.irModels(brand.slug, name, 0, 20);
      const hit = r.models.find((m) => m.name === name) ?? r.models[0];
      if (hit) await this.pickModel(hit.file, hit.name);
      else this.dispatch({ t: 'fail', scope: 'models', code: 'not_found', msg: `No model called ${name} in the list.` });
    } catch (e) {
      this.fail('models', e);
    }
  }
  private retry() {
    const scope = this.w.error?.scope;
    this.dispatch({ t: 'retry' });
    if (scope === 'candidates') void this.loadCandidates();
    else if (scope === 'models') void this.loadModels(this.offset, this.mq);
    else if (scope === 'device' && this.w.model) void this.pickModel(this.w.model.file, this.w.model.name);
    else void this.loadBrands(this.q);
    if (scope === 'brands') this.w = { ...this.w, loading: false };
  }

  // ------------------------------------------------------------ keys: Space / Y / N in the power test
  private onKey = (e: KeyboardEvent) => {
    if (this.w.step !== 'test' || this.w.loading || !currentCandidate(this.w) || e.ctrlKey || e.metaKey || e.altKey) return;
    if ((e.composedPath()[0] as HTMLElement | undefined)?.matches?.('input, textarea, select, button')) return;
    const k = e.key.toLowerCase();
    if (k === ' ') {
      e.preventDefault();
      void this.sender?.fire();
    } else if (k === 'y') this.answer('yes');
    else if (k === 'n') this.answer('no');
  };
  private answer(a: 'yes' | 'no' | 'skip') {
    this.dispatch({ t: 'answer', answer: a });
    if (this.w.step === 'model') void this.loadModels(0, '');
  }

  // ------------------------------------------------------------ transitions
  private pickSource(s: 'archive' | 'paste' | 'ha') {
    this.dispatch({ t: 'source', source: s });
    if (s === 'archive') void this.loadBrands('');
    if (s === 'ha') {
      void import('../components/hh-ha-browser'); // the navigator is a lazy chunk, only this source needs it
      void this.ha.loadHa();
    }
  }
  private pickBrand(b: IrBrand) {
    try {
      localStorage.setItem(RECENT, JSON.stringify([b, ...readRecents().filter((r) => r.slug !== b.slug)].slice(0, 6)));
    } catch {
      /* private mode */
    }
    this.dispatch({ t: 'brand', brand: { name: b.name, slug: b.slug } });
  }
  private pickType(id: string) {
    this.dispatch({ t: 'type', type: id });
    void this.loadCandidates();
  }
  private back() {
    this.dispatch({ t: 'back' });
    if (this.w.step === 'model' && !this.models) void this.loadModels(0, this.mq);
    if (this.w.step === 'brand') void this.loadBrands(this.q);
  }
  private finalName(n: string): string {
    return (this.names[n] ?? n).trim();
  }
  private nameProblems(): Record<string, string> {
    const out: Record<string, string> = {};
    const seen = new Set<string>();
    for (const n of this.w.selected) {
      const f = this.finalName(n);
      const p = nameProblem({ id: 'x', commands: {} } as Device, f) ?? (seen.has(f) ? 'Another command has this name' : null);
      if (p) out[n] = p;
      seen.add(f);
    }
    return out;
  }
  /** Why the device cannot be added (hub limits), or null. */
  private capReason(ha: boolean, inputs?: Record<string, IrAction>): string | null {
    const cfg = this.store.config;
    return capProblem(cfg, 'device') ?? (ha ? null : (capProblem(cfg, 'command', this.w.selected.length) ?? capProblem(cfg, 'input', Object.keys(inputs ?? {}).length)));
  }
  private add() {
    const d = this.w.result?.device;
    if (!d) return;
    const dev0 = this.capReason(d.kind === 'ha', d.inputs);
    if (dev0) {
      this.store.toast(dev0, 'error');
      return;
    }
    const commands: Record<string, IrAction> = {};
    for (const n of this.w.selected) commands[this.finalName(n)] = (d.commands ?? {})[n] as IrAction;
    const name = trimBytes(this.devName.trim()) || d.name || d.id;
    const id = deviceIdFor(this.store.config, d.kind === 'ha' ? name : (d.id ?? name));
    const dev: Device = { ...d, id, name, commands, ...(this.devIcon ? { icon: this.devIcon } : {}) };
    this.store.replace({ ...this.store.config, devices: [...(this.store.config.devices ?? []), dev] });
    this.store.toast(`Added ${name} to the draft (${Object.keys(commands).length} commands). Nothing is applied yet.`, 'info', true);
    location.hash = hashFor('devices', id);
  }

  // ------------------------------------------------------------ steps
  private banner() {
    const e = this.w.error;
    if (!e) return nothing;
    if (isOffline(e))
      return html`<div class="banner bad" role="alert"><span><b>The IR archive can't be reached.</b> Brands you opened before still work. ${e.msg}</span><span class="sp"></span><button class="btn sm" @click=${() => this.retry()}>Retry</button></div>`;
    const text: Record<string, string> = {
      unknown_brand: 'That brand is not in the archive any more. Pick another one.',
      archive_too_large: 'That model has too many commands (over 2000) to import. Pick another model.',
    };
    return html`<div class="banner bad" role="alert"><span>${text[e.code] ?? e.msg}</span><span class="sp"></span><button class="btn sm" @click=${() => this.retry()}>Retry</button></div>`;
  }
  private source() {
    const ok = this.ha.haWritable;
    return html`<h1>What are you adding?</h1><p class="lead">Pick where its codes come from.</p>
      <div class="tiles">
        <button class="tile" @click=${() => this.pickSource('archive')}>${art('tv', 'art')}IR archive<small>Search thousands of remotes by brand and model</small></button>
        <button class="tile" @click=${() => this.pickSource('paste')}>${art('other', 'art')}Paste or upload codes<small>Pronto hex, a Flipper .ir file or a SmartIR file</small></button>
        <button class="tile" ?disabled=${!ok} title=${ok ? '' : 'This add-on version cannot store Home Assistant actions yet'} @click=${() => this.pickSource('ha')}>${art('streamer', 'art')}Home Assistant entity<small>${ok ? 'A media player or remote you already have' : 'Needs a newer add-on (ha_actions is off)'}</small></button>
        <a class="tile" id="add-bt-device" href=${hashFor('bluetooth', 'host')}>${icon('bluetooth', 'big')}Bluetooth device<small>Pair a TV box or phone; the hub types to it</small></a>
        <a class="tile" id="add-bt-remote" href=${hashFor('bluetooth', 'remote')}>${icon('bluetooth', 'big')}Bluetooth remote<small>Use a Bluetooth remote with your activities</small></a>
      </div>`;
  }
  private brand() {
    const rec = this.q ? [] : readRecents();
    const list = this.brands.filter((b) => !rec.some((r) => r.slug === b.slug));
    const tile = (b: IrBrand) => html`<button class="brand" role="option" @click=${() => this.pickBrand(b)}><b>${b.name}</b><span>${b.count.toLocaleString()} models</span></button>`;
    return html`<h1>Which brand?</h1><p class="lead">Type to search. Press Enter to take the first match.</p>
      <input class="search" type="search" aria-label="Search brands" placeholder="Sony, Samsung, Denon…" .value=${this.q} autofocus
        @input=${(e: Event) => { this.q = (e.target as HTMLInputElement).value; clearTimeout(this.timer); this.timer = setTimeout(() => void this.loadBrands(this.q.trim()), 200); }}
        @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter' && this.brands[0]) this.pickBrand(this.brands[0]); if (e.key === 'ArrowDown') (this.renderRoot.querySelector('.brand') as HTMLElement | null)?.focus(); }} />
      ${this.brandsErr ? html`<div class="banner bad" role="alert"><span>${isOffline({ code: this.brandsErr.code, msg: '', scope: 'brands' }) ? "The IR archive can't be reached. Brands you opened before still work." : this.brandsErr.message}</span><span class="sp"></span><button class="btn sm" @click=${() => void this.loadBrands(this.q)}>Retry</button></div>` : nothing}
      ${rec.length ? html`<h3>Recent</h3><div class="brands" role="listbox" aria-label="Recent brands">${rec.map(tile)}</div>` : nothing}
      <h3>${this.q ? 'Matches' : 'Most models'}</h3>
      <div class="brands" role="listbox" aria-label="Brands" @keydown=${(e: KeyboardEvent) => this.gridKey(e)}>${list.map(tile)}</div>
      ${!list.length && !this.brandsErr ? html`<p class="why">No brand matches "${this.q}". Try fewer letters, or paste codes instead.</p>` : nothing}
      <div class="center"><button class="btn" @click=${() => this.pickSource('paste')}>Paste a Pronto code or upload a file instead</button></div>`;
  }
  private gridKey(e: KeyboardEvent) {
    const items = [...this.renderRoot.querySelectorAll<HTMLElement>('.brand')];
    const i = items.indexOf(e.target as HTMLElement);
    const to = e.key === 'ArrowRight' ? i + 1 : e.key === 'ArrowLeft' ? i - 1 : e.key === 'ArrowDown' ? i + 4 : e.key === 'ArrowUp' ? i - 4 : -1;
    if (to >= 0 && items[to]) {
      e.preventDefault();
      items[to]?.focus();
    } else if (to < 0 && e.key === 'ArrowUp') (this.renderRoot.querySelector('.search') as HTMLElement | null)?.focus();
  }
  private type() {
    return html`<h1>What kind of device is your ${this.w.brand?.name}?</h1><p class="lead">This decides which codes we test first.</p>
      <div class="tiles">${KINDS.map((k) => html`<button class="tile" @click=${() => (k === 'other' ? (this.choosingOther = true) : this.pickType(KIND_TYPE[k]))}>${art(k, 'art')}${KIND_LABEL[k]}</button>`)}</div>
      ${this.choosingOther ? html`<div class="row center"><select aria-label="Device type" @change=${(e: Event) => (this.otherType = (e.target as HTMLSelectElement).value)}>${OTHER_TYPES.map(([id, n]) => html`<option value=${id} ?selected=${id === this.otherType}>${n}</option>`)}</select><button class="btn pri" @click=${() => { this.choosingOther = false; this.pickType(this.otherType); }}>Continue</button></div>` : nothing}`;
  }
  private test() {
    const w = this.w;
    const c = currentCandidate(w);
    const what = w.type === '1' ? 'TV' : 'device';
    const verb = c?.kind === 'on' ? 'turn on' : c?.kind === 'off' ? 'turn off' : 'turn on or off';
    return html`<h1>Point the hub at your ${w.brand?.name} ${what}</h1>
      ${this.banner()}
      ${w.loading ? html`<p class="lead" role="status">Looking for power codes…</p>` : nothing}
      ${c
        ? html`<div class="stage">
            <p class="lead" aria-live="polite">${candidateLabel(w)} · used by ${c.count} model${c.count === 1 ? '' : 's'}${c.models.length ? ` (${c.models.slice(0, 3).join(', ')}…)` : ''}</p>
            <hh-send-ir variant="big" .code=${c.action} name="power test code"></hh-send-ir>
            <div class="q">Did the ${what} ${verb}?</div>
            <div class="answers">
              <button class="btn pri" @click=${() => this.answer('yes')}>Yes<kbd>Y</kbd></button>
              <button class="btn" @click=${() => this.answer('no')}>No, next code<kbd>N</kbd></button>
              <button class="btn" @click=${() => void this.sender?.fire()}>Send again<kbd>Space</kbd></button>
            </div>
            <div class="tried" aria-label="Codes tried">${(w.candidates ?? []).map((_, i) => { const t = w.tried.find((x) => x.idx === i); return html`<span class="chip ${t?.result === 'yes' ? 'ok' : i === w.idx ? 'acc' : ''}" title=${t ? t.result : i === w.idx ? 'now' : 'not tried'}>${i + 1}${t?.result === 'no' ? ' ✗' : t?.result === 'yes' ? ' ✓' : ''}</span>`; })}</div>
            ${w.truncated ? html`<p class="why">The archive is large, so only part of it was checked. There may be more codes.</p>` : nothing}
            <div class="row center"><button class="btn" @click=${() => this.answer('skip')}>Skip this code</button><button class="btn" @click=${() => { this.dispatch({ t: 'skipTest' }); void this.loadModels(0, ''); }}>I know my model</button></div>
          </div>`
        : nothing}`;
  }
  private model() {
    const w = this.w;
    const m = this.models;
    const likely = likelyModels(w);
    const loading = w.loading && w.model;
    return html`<h1>Which model?</h1>
      <p class="lead">${w.confirmed ? 'The code that worked is shared by these models. Pick yours, or search below.' : w.exhausted ? 'No code reacted, or none was offered. Search for your model.' : 'Search for your model.'}</p>
      ${this.banner()}
      ${loading ? html`<p class="lead" role="status">Loading ${w.model?.name}…</p>` : nothing}
      ${likely.length ? html`<h3>Likely matches</h3><div class="likely">${likely.map((x) => html`<button class="btn" @click=${() => (x.file ? void this.pickModel(x.file, x.name) : void this.pickLikely(x.name))}>${x.name}</button>`)}</div>` : nothing}
      <input class="search" type="search" aria-label="Search models" placeholder="Model name or number" .value=${this.mq} @input=${(e: Event) => { this.mq = (e.target as HTMLInputElement).value; clearTimeout(this.timer); this.timer = setTimeout(() => void this.loadModels(0, this.mq.trim()), 250); }} />
      ${m
        ? html`<div class="models" role="list" aria-label="Models">${m.models.map((x) => html`<button class="model" role="listitem" ?disabled=${!!loading} @click=${() => void this.pickModel(x.file, x.name)}><b>${x.name}</b><span class="muted">${x.file.replace(/\.json$/, '') === x.name ? '' : x.file}</span></button>`)}</div>
          <div class="pager"><button class="btn sm" ?disabled=${m.offset === 0} @click=${() => void this.loadModels(Math.max(0, m.offset - PAGE), this.mq)}>Previous</button>
            <span class="muted">${m.total ? `${m.offset + 1}–${m.offset + m.models.length} of ${m.total.toLocaleString()}` : 'No models match'}</span>
            <button class="btn sm" ?disabled=${m.offset + PAGE >= m.total} @click=${() => void this.loadModels(m.offset + PAGE, this.mq)}>Next</button></div>`
        : w.error ? nothing : html`<p class="lead" role="status">Loading models…</p>`}`;
  }
  private review() {
    const w = this.w;
    const r = w.result as IrDeviceResult;
    const d = r.device;
    const cmds = Object.entries(d.commands ?? {});
    const by = new Map<Group, [string, IrAction][]>(GROUPS.map((g) => [g, []]));
    for (const c of cmds) by.get(groupCommand(c[0]) as Group)?.push(c);
    const probs = this.nameProblems();
    const bad = Object.keys(probs).length > 0;
    const ha = d.kind === 'ha';
    const pw = d.power;
    return html`<h1>Review ${d.name ?? 'the device'}</h1><p class="lead">Untick what you do not need. Click a name to change it. Nothing is sent to the hub until you apply.</p>
      <div class="review">
        <div>
          ${(['on', 'off', 'toggle'] as const).some((k) => pw?.[k]) ? html`<section class="grp"><h3>${stockIconSvg(commandIconId('power'))} Power</h3><div class="cmds">${(['on', 'off', 'toggle'] as const).filter((k) => pw?.[k]).map((k) => html`<div class="cmd"><span class="m"><b>Power ${k}</b><small>${summarize(pw?.[k] as IrAction)}</small></span>${ha ? nothing : html`<hh-send-ir variant="round" .code=${pw?.[k]} name="power ${k}"></hh-send-ir>`}</div>`)}</div></section>` : nothing}
          ${GROUPS.filter((g) => (by.get(g)?.length ?? 0) > 0).map((g) => html`<section class="grp"><h3>${stockIconSvg(commandIconId(g, g))} ${pretty(g)}</h3><div class="cmds">${(by.get(g) ?? []).map(([n, code]) => {
            const on = w.selected.includes(n);
            return html`<div class="cmd ${on ? '' : 'off'}"><input type="checkbox" aria-label="Add ${n}" .checked=${on} @change=${() => this.dispatch({ t: 'toggle', name: n })} />
              <span class="m"><input type="text" aria-label="Name of ${n}" .value=${this.names[n] ?? n} @input=${(e: Event) => (this.names = { ...this.names, [n]: (e.target as HTMLInputElement).value })} />${probs[n] ? html`<span class="err" role="alert">${probs[n]}</span>` : html`<small>${code.ha ? `${code.ha.service}` : summarize(code)}</small>`}</span>
              ${code.ha ? nothing : html`<hh-send-ir variant="round" .code=${code} name=${n}></hh-send-ir>`}</div>`;
          })}</div></section>`)}
          ${Object.keys(d.inputs ?? {}).length ? html`<section class="grp"><h3>${stockIconSvg(commandIconId('input'))} Inputs</h3><div class="cmds">${Object.entries(d.inputs ?? {}).map(([n, code]) => html`<div class="cmd"><span class="m"><b>${n}</b><small>${code.ha ? code.ha.service : summarize(code)}</small></span>${code.ha ? nothing : html`<hh-send-ir variant="round" .code=${code} name=${n}></hh-send-ir>`}</div>`)}</div></section>` : nothing}
          ${cmds.length === 0 ? html`<p class="why">No commands yet. Add them on the device page after adding it.</p>` : nothing}
        </div>
        <aside class="side card" aria-label="Summary">
          <label>Device name<input class="namein" type="text" aria-label="Device name" .value=${this.devName} @input=${(e: Event) => (this.devName = trimBytes((e.target as HTMLInputElement).value))} /></label>
          <details><summary>Icon${this.devIcon ? `: ${this.devIcon}` : ' (default)'}</summary><hh-icon-picker .value=${this.devIcon} @hh-icon=${(e: CustomEvent<string | undefined>) => (this.devIcon = e.detail ?? '')}></hh-icon-picker></details>
          <span class="muted">${w.selected.length} of ${cmds.length} commands${ha ? '' : ' · settle ' + (d.settle_ms ?? 0) + ' ms'}</span>
          ${this.notes.length || (ha && (r.notes?.length ?? 0)) ? html`<ul class="plain">${(ha ? r.notes : this.notes).map((n) => html`<li>${n}</li>`)}</ul>` : nothing}
          ${r.skipped.length ? html`<div><b>Left out (${r.skipped_total || r.skipped.length})</b><ul class="plain">${r.skipped.slice(0, 8).map((s) => html`<li>${s.name}: ${s.reason}</li>`)}</ul></div>` : nothing}
          <button class="btn pri" ?disabled=${bad || !this.devName.trim() || !!this.capReason(ha, d.inputs)} @click=${() => this.add()}>Add device</button>
          ${this.capReason(ha, d.inputs) ? html`<span class="why" role="alert">${this.capReason(ha, d.inputs)}</span>` : nothing}
          <span class="why">Adds to the draft only. Review and apply it from the bar at the top.</span>
        </aside>
      </div>`;
  }
  private paste() {
    const p = this.parsed;
    return html`<h1>Paste or upload codes</h1><p class="lead">Pronto hex, a Flipper .ir file or a SmartIR .json file.</p>
      <div class="stage">
        <div class="row center"><select aria-label="Format" @change=${(e: Event) => (this.kind = (e.target as HTMLSelectElement).value as typeof this.kind)}><option value="pronto" ?selected=${this.kind === 'pronto'}>Pronto hex</option><option value="flipper" ?selected=${this.kind === 'flipper'}>Flipper .ir</option><option value="smartir" ?selected=${this.kind === 'smartir'}>SmartIR .json</option></select>
          <input type="file" aria-label="Upload a file" accept=".ir,.json,.txt" @change=${async (e: Event) => { const f = (e.target as HTMLInputElement).files?.[0]; if (f) { this.text = await f.text(); this.kind = f.name.endsWith('.ir') ? 'flipper' : f.name.endsWith('.json') ? 'smartir' : this.kind; } }} /></div>
        <textarea aria-label="Pasted codes" .value=${this.text} @input=${(e: Event) => (this.text = (e.target as HTMLTextAreaElement).value)}></textarea>
        <button class="btn pri" ?disabled=${!this.text.trim()} @click=${() => void this.readPaste()}>Read codes</button>
        ${this.parseErr ? html`<span class="err" role="alert">${this.parseErr}</span>` : nothing}
        ${p && !p.actions.length ? html`<span class="err">No usable codes found.</span>` : nothing}
      </div>`;
  }
  private async readPaste() {
    this.parseErr = '';
    try {
      const r = await this.api.irParse(this.kind, this.text);
      this.parsed = r;
      const commands: Record<string, IrAction> = {};
      for (const a of r.actions) commands[trimBytes(a.name)] = a.action;
      if (!r.actions.length) return;
      this.names = {};
      this.devName = 'Imported device';
      this.notes = [`${r.actions.length} code${r.actions.length === 1 ? '' : 's'} read${r.skipped.length ? `, ${r.skipped.length} skipped` : ''}.`];
      this.dispatch({ t: 'draft', result: { device: { id: 'imported', name: 'Imported device', commands }, forms: {}, notes: this.notes, skipped: r.skipped, skipped_total: r.skipped.length, device_type: null, source: {} } });
    } catch (e) {
      this.parseErr = e instanceof ApiError ? (e.errors[0]?.msg ?? e.message) : 'Could not read that.';
    }
  }
  private entity() {
    return html`<h1>Pick a Home Assistant entity</h1><p class="lead">We propose commands from what the entity's services can do. We cannot read its source list or remote presets, so those are left for you to fill in.</p>
      <hh-ha-browser mode="select" scope="wizard" label="media players, remotes, switches, lights and fans" .entities=${this.ha.entities} .loaded=${this.ha.haLoaded} .loading=${this.ha.haLoading} .reason=${this.ha.haReason}
        .registries=${this.ha.haRegistries} .lookups=${this.ha.haLookups} .truncated=${this.ha.haTruncated} .domains=${PROPOSAL_DOMAINS}
        @hh-ha-pick=${(e: CustomEvent<{ entity_id: string; name?: string }>) => this.useEntity(e.detail)} @hh-ha-retry=${() => void this.ha.loadHa(true)}></hh-ha-browser>`;
  }
  private useEntity(e: { entity_id: string; name?: string }) {
    const p = proposeFromEntity(this.store.config, e);
    if (!p) return;
    this.names = {};
    this.devName = trimBytes(p.device.name ?? '');
    this.notes = p.notes;
    this.dispatch({ t: 'draft', result: { device: p.device, forms: {}, notes: p.notes, skipped: [], skipped_total: 0, device_type: null, source: {} } });
  }

  protected override render() {
    const w = this.w;
    const steps = stepsFor(w.source);
    const at = progressIndex(w);
    return html`
      <div class="top">
        ${w.step === 'source' ? html`<a class="btn ghost" href=${hashFor('devices')}>← Devices</a>` : html`<button class="btn ghost" @click=${() => this.back()}>← Back</button>`}
        <ol class="steps" aria-label="Progress">${steps.map((s, i) => html`<li class=${i < at ? 'done' : i === at ? 'now' : ''} aria-current=${i === at ? 'step' : 'false'}>${s.label}</li>`)}</ol>
        <a class="btn ghost" href=${hashFor('devices')}>Cancel</a>
      </div>
      ${w.step === 'source' ? this.source() : w.step === 'brand' ? this.brand() : w.step === 'type' ? this.type() : w.step === 'test' ? this.test() : w.step === 'model' ? this.model() : w.step === 'paste' ? this.paste() : w.step === 'entity' ? this.entity() : this.review()}`;
  }
}
