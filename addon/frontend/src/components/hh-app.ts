import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { icon } from '../lib/icons';
import { counts } from '../lib/model';
import { Router, hashFor } from '../lib/router';
import { shared } from '../lib/styles';
import { ApiError, type HubInfo } from '../lib/api';
import { StoreElement, type ConfirmRequest } from './base';
import './hh-apply-dialog';
import './hh-dialog';
import './hh-draft-bar';
import './hh-import-dialog';
import '../views/hh-devices';
import '../views/hh-history';
import '../views/hh-overview';
import '../views/hh-placeholder';

interface RailItem {
  id: string;
  label: string;
  icon: string;
  count?: number;
}

const NOT_BUILT: Record<string, [string, string]> = {
  activities: ['Activities', 'Activities will be edited visually: pick devices and inputs, then arrange start and stop steps.'],
  keys: ['Hard keys', 'Hard keys will be assigned on a drawing of the remote itself.'],
  pages: ['Remote pages', 'Remote pages will be laid out on a preview of the remote screen.'],
  ir: ['IR library', 'The IR archive browser and add-device wizard need API endpoints that do not exist yet.'],
};

@customElement('hh-app')
export class HhApp extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: block; min-height: 100vh; }
      .skip { position: absolute; left: 8px; top: -48px; z-index: 20; background: var(--acc); color: var(--onacc); padding: 8px 12px; border-radius: var(--r1); }
      .skip:focus { top: 8px; }
      header { height: 48px; display: flex; align-items: center; gap: 12px; padding: 0 16px; background: var(--sf); border-bottom: 1px solid var(--line); position: sticky; top: 0; z-index: 7; }
      .brand { font-weight: 650; display: flex; gap: 8px; align-items: center; }
      .brand .ic { color: var(--acc); }
      .menu { display: none; }
      select { height: 32px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 8px; }
      .layout { display: grid; grid-template-columns: 216px minmax(0, 1fr); min-height: calc(100vh - 48px); }
      nav { background: var(--sf); border-right: 1px solid var(--line); padding: 12px 8px; display: flex; flex-direction: column; gap: 2px; }
      nav a { display: flex; align-items: center; gap: 10px; min-height: 36px; padding: 0 10px; border-radius: var(--r1); color: var(--tx2); text-decoration: none; font-weight: 500; }
      nav a:hover { background: var(--sf2); }
      nav a[aria-current="page"] { background: var(--accs); color: var(--tx); }
      nav a[aria-current="page"] .ic { color: var(--acc); }
      nav .n { margin-left: auto; font-size: 12px; color: var(--tx3); }
      nav .err { margin-left: auto; min-width: 18px; height: 18px; border-radius: 9px; background: var(--bad); color: #fff; font-size: 11px; font-weight: 700; display: grid; place-items: center; padding: 0 5px; }
      nav hr { border: 0; border-top: 1px solid var(--line); margin: 8px 4px; width: calc(100% - 8px); }
      .col { min-width: 0; display: flex; flex-direction: column; }
      main { padding: 20px 24px 32px; display: flex; flex-direction: column; gap: 16px; min-width: 0; outline: none; }
      .toasts { position: fixed; right: 16px; bottom: 16px; display: grid; gap: 8px; z-index: 30; max-width: min(380px, calc(100vw - 32px)); }
      .toast { display: flex; gap: 8px; align-items: flex-start; background: var(--tx); color: var(--bg); padding: 10px 12px; border-radius: var(--r2); box-shadow: var(--sh); }
      .toast.error { background: var(--bad); color: #fff; }
      .toast button { background: none; border: 0; color: inherit; cursor: pointer; padding: 0; margin-left: auto; }
      @media (max-width: 720px) {
        .menu { display: inline-flex; }
        .layout { grid-template-columns: minmax(0, 1fr); }
        nav { display: none; position: fixed; top: 48px; left: 0; bottom: 0; width: min(280px, 86vw); z-index: 8; box-shadow: var(--sh); border-right: 1px solid var(--line); overflow: auto; }
        nav.open { display: flex; }
        main { padding: 16px 16px 96px; }
        header { padding: 0 8px 0 8px; gap: 4px; }
        .hdr-btn span { display: none; }
        .toasts { bottom: 76px; }
      }
    `,
  ];
  private router = new Router();
  @state() private railOpen = false;
  @state() private applyOpen = false;
  @state() private importOpen = false;
  @state() private confirm: ConfirmRequest | null = null;
  @state() private hubs: HubInfo[] = [];

  override connectedCallback(): void {
    super.connectedCallback();
    this.router.addEventListener('change', () => {
      this.railOpen = false;
      this.requestUpdate();
    });
    this.addEventListener('hh-review', () => (this.applyOpen = true));
    this.addEventListener('hh-import', () => (this.importOpen = true));
    this.addEventListener('hh-close', () => {
      this.applyOpen = false;
      this.importOpen = false;
    });
    this.addEventListener('hh-confirm', (e) => {
      e.preventDefault();
      this.confirm = (e as CustomEvent<ConfirmRequest>).detail;
    });
    void this.loadHubs();
  }

  private async loadHubs() {
    try {
      this.hubs = (await this.store.api.hubs()).hubs;
    } catch (e) {
      if (!(e instanceof ApiError)) throw e; // endpoint not there yet: single hub
    }
  }

  private answer(yes: boolean) {
    this.confirm?.resolve(yes);
    this.confirm = null;
  }

  private rail(): RailItem[] {
    const c = counts(this.store.config);
    const hist = this.store.history.length + (this.store.applied ? 1 : 0);
    return [
      { id: 'overview', label: 'Overview', icon: 'overview' },
      { id: 'devices', label: 'Devices', icon: 'devices', count: c.devices },
      { id: 'activities', label: 'Activities', icon: 'activities', count: c.activities },
      { id: 'keys', label: 'Hard keys', icon: 'keys', count: c.keys },
      { id: 'pages', label: 'Remote pages', icon: 'pages', count: c.pages },
      { id: '-', label: '', icon: '' },
      { id: 'ir', label: 'IR library', icon: 'ir' },
      { id: 'history', label: 'History', icon: 'history', count: hist },
    ];
  }

  private view() {
    const { section, parts } = this.router.route;
    switch (section) {
      case 'overview':
        return html`<hh-overview></hh-overview>`;
      case 'devices':
        return html`<hh-devices .selected=${parts[0] ?? ''}></hh-devices>`;
      case 'history':
        return html`<hh-history></hh-history>`;
      default: {
        const [h, n] = NOT_BUILT[section] ?? ['', ''];
        return html`<hh-placeholder heading=${h} note=${n}></hh-placeholder>`;
      }
    }
  }

  protected override render() {
    const s = this.store;
    const sec = this.router.route.section;
    const errs = s.errorSummary.bySection;
    return html`
      <a class="skip" href="#main" @click=${(e: Event) => { e.preventDefault(); this.shadowRoot?.querySelector('main')?.focus(); }}>Skip to content</a>
      <header>
        <button class="btn ghost icon menu" aria-label="Menu" aria-expanded=${this.railOpen} @click=${() => (this.railOpen = !this.railOpen)}>${icon('menu')}</button>
        <span class="brand">${icon('hub')} Harmony Editor</span>
        ${this.hubs.length > 1
          ? html`<select aria-label="Hub" @change=${(e: Event) => { const u = new URL(location.href); u.searchParams.set('hub', (e.target as HTMLSelectElement).value); location.href = u.toString(); }}>
              ${this.hubs.map((h) => html`<option value=${h.id} ?selected=${h.id === s.hub}>${h.id}${h.has_applied ? '' : ' (no config yet)'}</option>`)}
            </select>`
          : nothing}
        <span class="sp"></span>
        <button class="btn ghost hdr-btn" aria-label="Import YAML" @click=${() => (this.importOpen = true)}>${icon('upload')}<span>Import YAML</span></button>
        <a class="btn ghost hdr-btn" aria-label="Export YAML" aria-disabled=${s.isFirstRun ? 'true' : 'false'} href=${s.isFirstRun ? '#/overview' : s.api.exportUrl(s.hub)} download="harmony-${s.hub}.yaml">${icon('download')}<span>Export YAML</span></a>
      </header>
      <div class="layout">
        <nav aria-label="Sections" class=${classMap({ open: this.railOpen })}>
          ${this.rail().map((r) =>
            r.id === '-'
              ? html`<hr />`
              : html`<a href=${hashFor(r.id)} aria-current=${sec === r.id ? 'page' : 'false'}>${icon(r.icon)}${r.label}
                  ${errs[r.id] ? html`<span class="err" aria-label="${errs[r.id]} errors">${errs[r.id]}</span>` : r.count !== undefined ? html`<span class="n">${r.count}</span>` : nothing}</a>`,
          )}
        </nav>
        <div class="col">
          <hh-draft-bar></hh-draft-bar>
          <main id="main" tabindex="-1">
            ${s.saveState === 'conflict'
              ? html`<div class="banner ir" role="alert">
                  <span>Changed in another tab or session.</span><span class="sp"></span>
                  <button class="btn sm" @click=${() => void s.reloadFromServer()}>Reload</button>
                  <button class="btn sm danger" @click=${() => void s.overwrite()}>Overwrite</button>
                </div>`
              : nothing}
            ${s.loadError
              ? html`<div class="banner bad" role="alert"><span>Could not load the config: ${s.loadError}</span><span class="sp"></span><button class="btn sm" @click=${() => void s.load()}>Retry</button></div>`
              : nothing}
            ${s.errorSummary.unlocated.length
              ? html`<div class="banner bad" role="alert">${s.errorSummary.unlocated.map((u) => html`<span>${u.msg}</span>`)}</div>`
              : nothing}
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
        ${s.toasts.map((t) => html`<div class="toast ${t.kind}"><span>${t.text}</span><button aria-label="Dismiss" @click=${() => s.dismissToast(t.id)}>${icon('x')}</button></div>`)}
      </div>
    `;
  }
}
