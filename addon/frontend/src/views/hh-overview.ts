import { css, html, nothing } from 'lit';
import { customElement } from 'lit/decorators.js';
import { displayKey } from '../lib/key-names';
import '../components/hh-chain-mini';
import '../components/hh-mini-remote';
import { activeTheme } from '../lib/theme-model';
import { StoreElement } from '../components/base';
import '../components/hh-page-diff';
import { changedKeys, changedPages, type KeyChange } from '../lib/diff';
import { LIMITS, counts } from '../lib/model';
import { byDomain, permissions } from '../lib/permissions';
import { keyTitle } from '../lib/remote-layout';
import { hashFor } from '../lib/router';
import { StudioState } from '../lib/studio-state';
import { pageIndexForLayer } from '../lib/studio-model';
import { shared } from '../lib/styles';

const KIND_LABEL = { added: 'Added', changed: 'Changed', removed: 'Removed' } as const;

/** Overview: the remote as it will look, the activities, what changed since the last apply, and what Home Assistant is asked for. */
@customElement('hh-overview')
export class HhOverview extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      .stack { display: grid; gap: 16px; min-width: 0; }
      h3 { margin: 14px 0 6px; font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); }
      .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; }
      .stat { padding: 12px 14px; }
      .stat .v { font-size: 22px; font-weight: 650; }
      .stat .l { font-size: 12px; color: var(--tx3); }
      .top { display: grid; grid-template-columns: 240px minmax(0, 1fr); gap: 16px; align-items: start; }
      .rem { display: grid; gap: 10px; justify-items: center; text-align: center; }
      .rem hh-mini-remote { width: 150px; filter: drop-shadow(0 6px 14px rgba(16, 24, 32, 0.25)); }
      .acts { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 10px; }
      a.act { display: grid; gap: 6px; padding: 12px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); color: var(--tx); text-decoration: none; }
      a.act:hover { border-color: var(--acc); }
      a.act b { display: flex; gap: 8px; align-items: center; font-size: 14px; }
      h2 .sub { font-weight: 400; color: var(--tx3); font-size: 12.5px; }
      .pages { display: grid; gap: 14px; }
      .chg { display: flex; gap: 24px; flex-wrap: wrap; align-items: flex-start; }
      .kk h3 { margin-top: 0; }
      .pg { display: grid; grid-template-columns: auto auto minmax(0, 1fr); gap: 12px; align-items: center; }
      .pg figure { margin: 0; display: grid; gap: 4px; justify-items: center; font-size: 11px; color: var(--tx3); }
      .pg svg { border-radius: 8px; box-shadow: 0 0 0 1px var(--line); display: block; }
      .arrow { color: var(--tx3); font-size: 18px; }
      .xr { display: flex; gap: 18px; flex-wrap: wrap; }
      .xr figure { margin: 0; display: grid; gap: 4px; justify-items: center; font-size: 12px; font-weight: 600; width: 96px; }
      .xr figure hh-mini-remote { width: 96px; }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 6px; }
      li.row { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      .empty { text-align: center; padding: 48px 16px; display: grid; gap: 12px; justify-items: center; }
      .empty h1 { font-size: 24px; }
      .empty p { max-width: 46ch; color: var(--tx2); margin: 0; }
      .empty .row { display: flex; gap: 8px; flex-wrap: wrap; justify-content: center; }
      .perm { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
      .scrollbox { max-height: 280px; overflow: auto; border: 1px solid var(--line); border-radius: var(--r1); padding: 4px 10px; }
      .perm h3 { margin: 8px 0 2px; font-size: 11.5px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--tx3); }
      .perm ul { margin: 0; padding: 0; list-style: none; }
      .perm li { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
      .perm .why { color: var(--tx3); font-size: 12px; }
      .svc { font-size: 11px; padding: 1px 7px; border-radius: 9px; border: 1px solid var(--line); color: var(--tx2); }
      .st { font-size: 11px; padding: 1px 7px; border-radius: 9px; background: var(--sf3); color: var(--tx2); }
      .st.on { background: var(--irs); color: var(--ir); }
      a.fix { color: var(--bad); font-weight: 600; }
      @media (max-width: 760px) { .top { grid-template-columns: minmax(0, 1fr); } .pg { grid-template-columns: auto auto; } .pg > :last-child { grid-column: 1 / -1; } }
    `,
  ];
  private ha = new StudioState(this.store);

  override connectedCallback(): void {
    super.connectedCallback();
    this.ha.addEventListener('change', this.onHa);
    this.ha.haWithState = true;
    void this.ha.loadHa();
  }
  override disconnectedCallback(): void {
    this.ha.removeEventListener('change', this.onHa);
    super.disconnectedCallback();
  }
  private onHa = () => this.requestUpdate();

  private keyRemotes(changes: KeyChange[]) {
    const layers = [...new Set(changes.map((c) => c.layer))];
    return html`<div class="xr">${layers.slice(0, 4).map((l) => {
      const mine = changes.filter((c) => c.layer === l);
      const marks = Object.fromEntries(mine.map((c) => [c.key, 'chg']));
      return html`<figure><hh-mini-remote .marks=${marks} label="Keys changed in ${mine[0]?.layerName}"></hh-mini-remote>${mine[0]?.layerName}</figure>`;
    })}</div>`;
  }

  private permissions() {
    const p = permissions(this.store.config);
    const live = new Map((this.ha.entities ?? []).map((e) => [e.entity_id, e]));
    return html`<section class="card" aria-labelledby="hp">
      <h2 id="hp">Permissions <span class="sub">Home Assistant entities and services this draft uses</span></h2>
      ${p.entities.length || p.services.length
        ? html`<div class="perm">
            <div><b>Entities (${p.entities.length})</b><div class="scrollbox" tabindex="0" role="region" aria-label="Entities by domain">${byDomain(p).map((g) => html`<h3>${displayKey(g.domain)} (${g.entities.length})</h3><ul>${g.entities.map((e) => html`<li><span class="mono">${e.id}</span>${live.get(e.id)?.state ? html`<span class="st ${live.get(e.id)?.state === 'on' ? 'on' : ''}">${live.get(e.id)?.state}</span>` : nothing}${e.services.length ? html`<span class="svc">${e.services.map((x) => x.split('.').pop()).join(', ')}</span>` : nothing}<span class="why" title=${e.uses.join(', ')}>${e.uses.slice(0, 2).join(', ')}</span></li>`)}</ul>`)}</div></div>
            <div><b>Services (${p.services.length})</b>${p.services.length ? html`<div class="scrollbox"><ul>${p.services.map((s) => html`<li><span class="mono">${s.service}</span><span class="why">${s.uses} use${s.uses === 1 ? '' : 's'}</span></li>`)}</ul></div>` : html`<p class="muted">No service calls.</p>`}</div>
          </div>`
        : html`<p class="muted">The draft does not ask Home Assistant for anything.</p>`}
      <p class="muted">Informational. Nothing is granted here; Home Assistant decides what the integration may do.</p>
    </section>`;
  }

  protected override render() {
    const s = this.store;
    if (s.isFirstRun) {
      return html`<div class="card empty">
        <h1>Set Up Your First Hub Config</h1>
        <p>Nothing has been applied yet. Import a YAML file you already have, or start by adding a device.</p>
        <div class="row">
          <button class="btn" @click=${() => this.dispatchEvent(new Event('hh-import', { bubbles: true, composed: true }))}>Import YAML File</button>
          <a class="btn pri" href=${hashFor('devices', 'new')}>Add Your First Device</a>
        </div>
      </div>`;
    }
    const c = counts(s.config);
    const acts = s.config.activities ?? [];
    const applied = s.appliedConfig;
    const diff = s.diff;
    const errs = s.errorSummary;
    const home = pageIndexForLayer(s.config, 'default');
    const homePage = s.pages[home]?.page ?? null;
    const pages = s.hasDraft ? changedPages(applied, s.config) : [];
    const keys = s.hasDraft ? changedKeys(applied, s.config) : [];
    const changedActs = new Set(acts.filter((a) => s.hasDraft && JSON.stringify(a) !== JSON.stringify(applied?.activities?.find((x) => x.id === a.id))).map((a) => a.id));
    return html`
      <h1>Overview</h1>
      ${errs.total
        ? html`<div class="banner bad" role="alert">
            <span>${errs.total} error${errs.total === 1 ? '' : 's'} stop this draft from being applied. First: <span class="mono">${errs.first?.path || 'config'}</span> ${errs.first?.msg}</span>
            <span class="sp"></span>
            ${errs.first ? html`<a class="btn sm" href=${'#/' + errs.first.route}>Go to Field</a>` : nothing}
          </div>`
        : nothing}
      <div class="top">
        <section class="card rem" aria-label="Your remote">
          <h2>Your Remote</h2>
          <hh-mini-remote .page=${homePage} .theme=${activeTheme(s.config).theme} label="Your remote showing the ${s.config.pages?.[home]?.title ?? 'home'} page"></hh-mini-remote>
          <a class="btn sm" href=${hashFor('remote')}>Open in the Studio</a>
        </section>
          <div class="stack">
            <div class="stats">
              ${this.stat('Devices', c.devices, LIMITS.devices)} ${this.stat('Activities', c.activities, LIMITS.activities)}
              ${this.stat('Page widgets', c.widgets, LIMITS.widgets)} ${this.stat('Inputs', c.inputs, LIMITS.inputs)}
            </div>
            <section class="card" aria-labelledby="ha">
              <h2 id="ha">Activities</h2>
              ${acts.length
                ? html`<div class="acts">${acts.map((a) => html`<a class="act" href=${hashFor('activities', a.id)}><b>${a.name ?? a.id}${changedActs.has(a.id) ? html`<span class="chip ir">Changed</span>` : nothing}</b><hh-chain-mini .cfg=${s.config} activity=${a.id}></hh-chain-mini><span class="muted">${(a.steps ?? []).length + (a.off ?? []).length} own steps · ${Object.keys(a.keys ?? {}).length} keys</span></a>`)}</div>`
                : html`<p class="muted">No activities yet. <a href=${hashFor('activities')}>Create One</a>.</p>`}
            </section>
      <section class="card" aria-labelledby="hc">
              <h2 id="hc">${applied ? `Since Rev ${s.baseRev}` : 'Changes in This Draft'}</h2>
              ${diff.length
                ? html`
                  <div class="chg">${pages.length ? html`<hh-page-diff></hh-page-diff>` : nothing}
                  ${keys.length ? html`<div class="kk"><h3>Keys</h3>${this.keyRemotes(keys)}</div>` : nothing}</div>
            ${keys.length ? html`<ul>${keys.slice(0, 10).map((k) => html`<li class="row"><span class="k ${k.kind}">${KIND_LABEL[k.kind]}</span><span>${keyTitle(k.key)} <span class="muted">in ${k.layerName}</span></span></li>`)}</ul>` : nothing}
                  <h3>Everything That Changed</h3>
                  <ul>${diff.map((r) => html`<li class="row"><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</li>`)}</ul>`
                : html`<p class="muted">${s.hasDraft ? 'The draft matches the applied config.' : 'No draft. Edits you make appear here.'}</p>`}
            </section>
          </div>
      </div>
      ${this.permissions()}
    `;
  }

  private stat(label: string, n: number, max: number) {
    return html`<div class="card stat"><div class="v">${n}${Number.isFinite(max) ? html`<span class="muted"> / ${max}</span>` : nothing}</div><div class="l">${label}</div></div>`;
  }
}
