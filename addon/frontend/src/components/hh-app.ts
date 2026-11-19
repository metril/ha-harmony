import { css, html, nothing, type TemplateResult } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { icon } from '../lib/icons';
import { counts } from '../lib/model';
import { Router, hashFor } from '../lib/router';
import { shared } from '../lib/styles';
import { ApiError, type ApplyStatus, type HubsResponse } from '../lib/api';
import { hubLabel, hubName, linkInfo, linkProblem, pickHub, rememberHub } from '../lib/hubs';
import { StoreElement, type ConfirmRequest } from './base';
import './hh-dialog';
import './hh-draft-bar';
import '../views/hh-studio';

interface RailItem {
  id: string;
  label: string;
  icon: string;
  count?: number;
}

@customElement('hh-app')
export class HhApp extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: block; min-height: 100vh; }
      .skip { position: absolute; left: 8px; top: -48px; z-index: 20; background: var(--acc); color: var(--onacc); padding: 8px 12px; border-radius: var(--r1); border: 0; cursor: pointer; }
      .skip:focus { top: 8px; }
      h1.vh { margin: 0; }
      header { height: 48px; display: flex; align-items: center; gap: 12px; padding: 0 16px; background: var(--sf); border-bottom: 1px solid var(--line); position: sticky; top: 0; z-index: 7; }
      .brand { font-weight: 650; display: flex; gap: 8px; align-items: center; }
      .brand .ic { color: var(--acc); }
      .menu { display: none; }
      .hub { display: flex; align-items: center; gap: 6px; min-width: 0; padding-left: 12px; border-left: 1px solid var(--line); height: 28px; }
      .hub .hl { color: var(--tx3); font-size: 12px; }
      .hub .hid { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 16ch; font-family: var(--mono); font-size: 12px; }
      .hub .hn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 18ch; font-weight: 600; }
      .hub .hid2 { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 16ch; font-family: var(--mono); font-size: 11px; color: var(--tx3); }
      .hub select { max-width: 22ch; }
      select { height: 32px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 8px; }
      .layout { display: grid; grid-template-columns: 216px minmax(0, 1fr); min-height: calc(100vh - 48px); }
      @media (min-width: 721px) {
        .layout.slim { grid-template-columns: 56px minmax(0, 1fr); }
        .layout.slim nav { padding: 12px 6px; align-items: center; }
        .layout.slim nav a { width: 44px; justify-content: center; padding: 0; position: relative; }
        .layout.slim nav a .lbl, .layout.slim nav a .n { display: none; }
        .layout.slim nav a .err { position: absolute; top: 2px; right: 0; min-width: 16px; height: 16px; font-size: 10px; }
      }
      .collapse { margin-top: auto; }
      @media (max-width: 720px) { .collapse { display: none; } }
      nav { background: var(--sf); border-right: 1px solid var(--line); padding: 12px 8px; display: flex; flex-direction: column; gap: 2px; }
      nav a { display: flex; align-items: center; gap: 10px; min-height: 36px; padding: 0 10px; border-radius: var(--r1); color: var(--tx2); text-decoration: none; font-weight: 500; }
      nav a:hover { background: var(--sf2); }
      nav a[aria-current="page"] { background: var(--accs); color: var(--tx); }
      nav a[aria-current="page"] .ic { color: var(--acc); }
      nav .n { margin-left: auto; font-size: 12px; color: var(--tx3); }
      nav .err { margin-left: auto; min-width: 18px; height: 18px; border-radius: 9px; background: var(--bad); color: #fff; font-size: 11px; font-weight: 700; display: grid; place-items: center; padding: 0 5px; }
      nav button.collapse { display: flex; align-items: center; gap: 10px; min-height: 36px; padding: 0 10px; border: 0; background: transparent; color: var(--tx2); border-radius: var(--r1); cursor: pointer; font-weight: 500; }
      nav button.collapse:hover { background: var(--sf2); }
      @media (min-width: 721px) { .layout.slim nav button.collapse { width: 44px; justify-content: center; padding: 0; }
      .layout.slim nav button.collapse .lbl { display: none; } }
      nav hr { border: 0; border-top: 1px solid var(--line); margin: 8px 4px; width: calc(100% - 8px); }
      .col { min-width: 0; display: flex; flex-direction: column; }
      main.wide { padding: 8px 12px 8px; }
      main { padding: 20px 24px 32px; display: flex; flex-direction: column; gap: 16px; min-width: 0; outline: none; }
      .toasts { position: fixed; right: 16px; bottom: 16px; display: grid; gap: 8px; z-index: 30; max-width: min(380px, calc(100vw - 32px)); }
      .toast { display: flex; gap: 8px; align-items: flex-start; background: var(--tx); color: var(--bg); padding: 10px 12px; border-radius: var(--r2); box-shadow: var(--sh); }
      .toast.error { background: var(--bad); color: #fff; }
      .toast .undo { font-weight: 700; text-decoration: underline; margin-left: 8px; }
      .toast button { background: none; border: 0; color: inherit; cursor: pointer; padding: 0; margin-left: auto; }
      @media (max-width: 720px) {
        .menu { display: inline-flex; }
        .layout { grid-template-columns: minmax(0, 1fr); }
        nav { display: none; position: fixed; top: 48px; left: 0; bottom: 0; width: min(280px, 86vw); z-index: 8; box-shadow: var(--sh); border-right: 1px solid var(--line); overflow: auto; }
        nav.open { display: flex; }
        main { padding: 16px 16px 96px; }
        header { padding: 0 8px 0 8px; gap: 4px; }
        .brand { display: none; }
        .hub { border-left: 0; padding-left: 4px; }
        .hub .hid { max-width: 10ch; }
        .hub .hn { max-width: 10ch; }
        .hub .hid2 { display: none; }
        .hub select { max-width: 12ch; }
        .hub .chip { padding: 0 6px; font-size: 11px; }
        .hdr-btn span { display: none; }
        .toasts { bottom: 76px; }
      }
    `,
  ];
  private router = new Router();
  @state() private railOpen = false;
  @state() private slim = ((): boolean => {
    try {
      const v = localStorage.getItem('hh-rail');
      if (v) return v === 'slim';
    } catch { /* storage unavailable */ }
    return typeof matchMedia === 'function' && matchMedia('(max-width: 1100px)').matches;
  })();
  private toggleSlim() {
    this.slim = !this.slim;
    try { localStorage.setItem('hh-rail', this.slim ? 'slim' : 'wide'); } catch { /* ignore */ }
  }
  @state() private applyOpen = false;
  @state() private importOpen = false;
  @state() private confirm: ConfirmRequest | null = null;
  @state() private hubs: HubsResponse | null = null;
  @state() private link: ApplyStatus | null = null;
  private linkTimer?: ReturnType<typeof setInterval>;

  override connectedCallback(): void {
    super.connectedCallback();
    this.router.addEventListener('change', () => {
      this.railOpen = false;
      this.requestUpdate();
    });
    this.addEventListener('hh-review', () => {
      void import('./hh-apply-dialog');
      this.applyOpen = true;
    });
    this.addEventListener('hh-import', () => this.openImport());
    this.addEventListener('hh-close', () => {
      this.applyOpen = false;
      this.importOpen = false;
    });
    this.addEventListener('hh-confirm', (e) => {
      e.preventDefault();
      this.confirm = (e as CustomEvent<ConfirmRequest>).detail;
    });
    void this.loadHubs();
    void this.loadLink();
    this.linkTimer = setInterval(() => void this.loadLink(), 15000);
  }
  override disconnectedCallback(): void {
    clearInterval(this.linkTimer);
    super.disconnectedCallback();
  }

  /** Applied vs acked rev and whether Home Assistant polls for this hub: visible after navigating away from the apply dialog. */
  private async loadLink() {
    try {
      this.link = await this.store.api.applyStatus(this.store.hub);
    } catch {
      /* transient */
    }
  }

  private goHub(hub: string) {
    rememberHub(hub);
    const u = new URL(location.href);
    u.searchParams.set('hub', hub);
    location.href = u.toString();
  }

  private async adopt(to: string) {
    try {
      await this.store.api.adoptHub(to);
      this.goHub(to);
    } catch (e) {
      this.store.toast(e instanceof Error ? e.message : String(e), 'error');
    }
  }

  private async loadHubs() {
    try {
      this.hubs = await this.store.api.hubs();
    } catch (e) {
      if (!(e instanceof ApiError)) throw e; // endpoint not there yet: single hub
    }
  }

  private answer(yes: boolean) {
    this.confirm?.resolve(yes);
    this.confirm = null;
  }

  private hubChoices(): string[] {
    const ids = pickHub(`?hub=${encodeURIComponent(this.store.hub)}`, this.hubs).choices;
    return ids.includes(this.store.hub) ? ids : [this.store.hub, ...ids];
  }

  /** Always-visible "Hub: id" with whether Home Assistant is polling for it; a switcher when several hubs exist. */
  private hubIndicator() {
    const s = this.store;
    const info = linkInfo(this.link, s.hub);
    const state: Record<string, [string, string, string]> = {
      polling: ['ok', 'Linked, polling', 'Home Assistant is polling for this hub.'],
      stale: ['ir', 'Not polling', 'Home Assistant is not polling for this hub right now.'],
      other: ['ir', 'Not polling', 'Home Assistant polls for a different hub id.'],
      none: ['', 'No Home Assistant linked', 'No Home Assistant integration is linked to this add-on yet.'],
    };
    const st = state[info.kind];
    const choices = this.hubChoices();
    return html`<div class="hub" role="group" aria-label="Hub">
      <span class="hl">Hub:</span>
      ${choices.length > 1
        ? html`<select aria-label="Hub" @change=${(e: Event) => this.goHub((e.target as HTMLSelectElement).value)}>
            ${choices.map((id) => html`<option value=${id} ?selected=${id === s.hub}>${hubLabel(this.hubs, id)}${this.hubs?.hubs.find((h) => h.id === id)?.has_applied ? '' : ' (no config yet)'}</option>`)}
          </select>`
        : hubName(this.hubs, s.hub)
          ? html`<strong class="hn" title=${hubName(this.hubs, s.hub)}>${hubName(this.hubs, s.hub)}</strong><span class="hid2" title=${s.hub}>${s.hub}</span>`
          : html`<strong class="hid" title=${s.hub}>${s.hub}</strong>`}
      ${st ? html`<span class="chip ${st[0]}" id="hub-state" title=${st[2]}>${st[1]}</span>` : nothing}
    </div>`;
  }

  private linkBanners() {
    const s = this.store;
    const out = [];
    const adopt = this.hubs?.adopt;
    if (adopt)
      out.push(html`<div class="banner ir" role="status"><span>The config under "${adopt.from}" is not the id Home Assistant polls for. Move it to:</span><span class="sp"></span>
        ${adopt.to.map((id) => html`<button class="btn sm" @click=${() => void this.adopt(id)}>${id}</button>`)}</div>`);
    const info = linkInfo(this.link, s.hub);
    const problem = linkProblem(info, s.hub);
    if (problem) out.push(html`<div class="banner ir" role="status" id="link-problem">${problem}</div>`);
    const l = this.link;
    if (!problem && l && l.applied_rev !== null && l.acked_rev !== l.applied_rev)
      out.push(html`<div class="banner" role="status" id="link-pending">${this.store.applied ? `Rev ${this.store.applied.rev}` : 'The new config'} is being sent to the remote…</div>`);
    return out;
  }

  private rail(): RailItem[] {
    const c = counts(this.store.config);
    const hist = this.store.history.length + (this.store.applied ? 1 : 0);
    return [
      { id: 'remote', label: 'Remote', icon: 'keys' },
      { id: 'overview', label: 'Overview', icon: 'overview' },
      { id: 'devices', label: 'Devices', icon: 'devices', count: c.devices },
      { id: 'activities', label: 'Activities', icon: 'activities', count: c.activities },
      { id: '-', label: '', icon: '' },
      { id: 'history', label: 'History', icon: 'history', count: hist },
    ];
  }

  private sectionTitle(): string {
    const names: Record<string, string> = { remote: 'Remote studio', overview: 'Overview', devices: 'Devices', ir: 'Add a device', activities: 'Activities', history: 'History' };
    return names[this.router.route.section] ?? 'Harmony Editor';
  }

  /** Dialogs load their module on first use; the tag is already in the DOM and upgrades when it arrives. */
  private openImport(): void {
    void import('./hh-import-dialog');
    this.importOpen = true;
  }

  private view() {
    const { section, parts } = this.router.route;
    switch (section) {
      case 'remote':
        return html`<hh-studio></hh-studio>`;
      case 'overview':
        return this.lazy('overview', () => import('../views/hh-overview'), html`<hh-overview></hh-overview>`);
      case 'devices':
        if (parts[0] === 'new') return this.lazy('add-device', () => import('../views/hh-add-device'), html`<hh-add-device></hh-add-device>`);
        return this.lazy('devices', () => import('../views/hh-devices'), html`<hh-devices .selected=${parts[0] ?? ''}></hh-devices>`);
      case 'ir':
        return this.lazy('add-device', () => import('../views/hh-add-device'), html`<hh-add-device></hh-add-device>`);
      case 'activities':
        return this.lazy('activities', () => import('../views/hh-activities'), html`<hh-activities .selected=${parts[0] ?? ''}></hh-activities>`);
      case 'history':
        return this.lazy('history', () => import('../views/hh-history'), html`<hh-history></hh-history>`);
      default:
        return nothing;
    }
  }
  private loaded = new Set<string>();
  /** Routes other than the landing studio load as separate chunks; the tag renders once its module has run. */
  private lazy(name: string, load: () => Promise<unknown>, tpl: TemplateResult) {
    if (this.loaded.has(name)) return tpl;
    void load().then(() => {
      this.loaded.add(name);
      this.requestUpdate();
    });
    return html`<p class="muted" role="status">Loading…</p>`;
  }

  protected override render() {
    const s = this.store;
    const sec = this.router.route.section;
    const errs = s.errorSummary.bySection;
    return html`
      <header>
        <button class="skip" @click=${() => this.shadowRoot?.querySelector('main')?.focus()}>Skip to content</button>
        <button class="btn ghost icon menu" aria-label="Menu" aria-expanded=${this.railOpen} @click=${() => (this.railOpen = !this.railOpen)}>${icon('menu')}</button>
        <span class="brand">${icon('hub')} Harmony Editor</span>
        ${this.hubIndicator()}
        <span class="sp"></span>
        <button class="btn ghost hdr-btn" aria-label="Import YAML" @click=${() => this.openImport()}>${icon('upload')}<span>Import YAML</span></button>
        <a class="btn ghost hdr-btn" aria-label="Export YAML" aria-disabled=${s.isFirstRun ? 'true' : 'false'} href=${s.isFirstRun ? '#/overview' : s.api.exportUrl(s.hub)} download="harmony-${s.hub}.yaml">${icon('download')}<span>Export YAML</span></a>
      </header>
      <div class=${classMap({ layout: true, slim: this.slim })}>
        <nav aria-label="Sections" class=${classMap({ open: this.railOpen })}>
          ${this.rail().map((r) =>
            r.id === '-'
              ? html`<hr />`
              : html`<a href=${hashFor(r.id)} aria-current=${sec === r.id ? 'page' : 'false'} title=${r.label} aria-label=${r.label + (errs[r.id] ? `, ${errs[r.id]} errors` : '')}>${icon(r.icon)}<span class="lbl">${r.label}</span>
                  ${errs[r.id] ? html`<span class="err" aria-hidden="true">${errs[r.id]}</span>` : r.count !== undefined ? html`<span class="n">${r.count}</span>` : nothing}</a>`,
          )}
          <button class="collapse" aria-pressed=${this.slim} title=${this.slim ? 'Expand the menu' : 'Collapse the menu'} aria-label=${this.slim ? 'Expand the menu' : 'Collapse the menu'} @click=${() => this.toggleSlim()}>${icon('collapse')}<span class="lbl">Collapse</span></button>
        </nav>
        <div class="col">
          <hh-draft-bar></hh-draft-bar>
          <main id="main" tabindex="-1" class=${classMap({ wide: sec === 'remote' })}>
            ${this.linkBanners()}
            ${s.saveState === 'conflict'
              ? html`<div class="banner ir" role="alert">
                  <span>Changed in another tab or session.</span><span class="sp"></span>
                  <button class="btn sm" @click=${() => void s.reloadFromServer()}>Reload</button>
                  <button class="btn sm danger" @click=${() => void s.overwrite()}>Overwrite</button>
                </div>`
              : nothing}
            ${s.stashed
              ? html`<div class="banner ir" role="alert">
                  <span>Restore unsaved changes from a moment ago? They were not saved before the page closed.</span><span class="sp"></span>
                  <button class="btn sm" @click=${() => s.restoreStash()}>Restore</button>
                  <button class="btn sm" @click=${() => s.discardStash()}>Discard</button>
                </div>`
              : nothing}
            ${s.loadError
              ? html`<div class="banner bad" role="alert"><span>Could not load the config: ${s.loadError}</span><span class="sp"></span><button class="btn sm" @click=${() => void s.load()}>Retry</button></div>`
              : nothing}
            ${s.errorSummary.unlocated.length
              ? html`<div class="banner bad" role="alert">${s.errorSummary.unlocated.map((u) => html`<span>${u.msg}</span>`)}</div>`
              : nothing}
            <h1 class="vh">${this.sectionTitle()}</h1>
            ${!s.loaded ? html`<p class="muted" role="status">Loading…</p>` : this.view()}
          </main>
        </div>
      </div>
      <hh-apply-dialog .open=${this.applyOpen}></hh-apply-dialog>
      <hh-import-dialog .open=${this.importOpen}></hh-import-dialog>
      <hh-dialog .open=${this.confirm !== null} heading=${this.confirm?.title ?? ''} @close=${() => this.answer(false)}>
        <p>${this.confirm?.body}</p>
        <button slot="footer" class="btn" @click=${() => this.answer(false)}>Cancel</button>
        <button slot="footer" class="btn ${this.confirm?.danger ? 'danger' : 'pri'}" @click=${() => this.answer(true)}>${this.confirm?.ok}</button>
      </hh-dialog>
      <div class="toasts" role="status" aria-live="polite">
        ${s.toasts.map((t) => html`<div class="toast ${t.kind}"><span>${t.text}</span>${t.undo ? html`<button class="undo" @click=${() => { s.undo(); s.dismissToast(t.id); }}>Undo</button>` : nothing}<button aria-label="Dismiss" @click=${() => s.dismissToast(t.id)}>${icon('x')}</button></div>`)}
      </div>
    `;
  }
}
