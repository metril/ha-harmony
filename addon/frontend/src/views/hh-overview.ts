import { css, html, nothing } from 'lit';
import { customElement } from 'lit/decorators.js';
import { StoreElement } from '../components/base';
import { LIMITS, counts } from '../lib/model';
import { shared } from '../lib/styles';

@customElement('hh-overview')
export class HhOverview extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; }
      .stat .v { font-size: 22px; font-weight: 650; }
      .stat .l { font-size: 12px; color: var(--tx3); }
      .two { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 6px; }
      li.row { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      .empty { text-align: center; padding: 48px 16px; display: grid; gap: 12px; justify-items: center; }
      .empty h1 { font-size: 24px; }
      .empty p { max-width: 46ch; color: var(--tx2); margin: 0; }
      .empty .row { display: flex; gap: 8px; flex-wrap: wrap; justify-content: center; }
      a.fix { color: var(--bad); font-weight: 600; }
    `,
  ];

  private permissions(): string[] {
    const ents = new Set<string>();
    for (const p of this.store.config.pages ?? [])
      for (const w of p.widgets ?? []) if (typeof w.entity === 'string') ents.add(w.entity);
    return [...ents].sort();
  }

  protected override render() {
    const s = this.store;
    if (s.isFirstRun) {
      return html`<div class="card empty">
        <h1>Set up your first hub config</h1>
        <p>Nothing has been applied yet. Import a YAML file you already have, or start by adding a device.</p>
        <div class="row">
          <button class="btn pri" @click=${() => this.dispatchEvent(new Event('hh-import', { bubbles: true, composed: true }))}>Import YAML file</button>
          <button class="btn" aria-disabled="true" title="The device editor is being rebuilt and is not available yet">Add first device <span class="chip">Coming</span></button>
        </div>
      </div>`;
    }
    const c = counts(s.config);
    const acts = s.config.activities ?? [];
    const diff = s.diff;
    const perms = this.permissions();
    const errs = s.errorSummary;
    return html`
      <h1>Overview</h1>
      ${errs.total
        ? html`<div class="banner bad" role="alert">
            <span>${errs.total} error${errs.total === 1 ? '' : 's'} stop this draft from being applied. First: <span class="mono">${errs.first?.path || 'config'}</span> ${errs.first?.msg}</span>
            <span class="sp"></span>
            ${errs.first ? html`<a class="btn sm" href=${'#/' + errs.first.route}>Go to field</a>` : nothing}
          </div>`
        : nothing}
      <div class="stats">
        ${this.stat('Devices', c.devices, LIMITS.devices)} ${this.stat('Activities', c.activities, 16)}
        ${this.stat('Page widgets', c.widgets, LIMITS.widgets)} ${this.stat('Inputs', c.inputs, LIMITS.inputs)}
      </div>
      <div class="two">
        <section class="card" aria-labelledby="ha">
          <h2 id="ha">Activities</h2>
          ${acts.length
            ? html`<ul>${acts.map((a) => html`<li>${a.name ?? a.id} <span class="muted">${(a.devices ?? []).length} devices</span></li>`)}</ul>`
            : html`<p class="muted">No activities yet.</p>`}
        </section>
        <section class="card" aria-labelledby="hc">
          <h2 id="hc">Changes in this draft</h2>
          ${diff.length
            ? html`<ul>${diff.map((r) => html`<li class="row"><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</li>`)}</ul>`
            : html`<p class="muted">${s.hasDraft ? 'The draft matches the applied config.' : 'No draft. Edits you make appear here.'}</p>`}
        </section>
      </div>
      <section class="card" aria-labelledby="hp">
        <h2 id="hp">Permissions</h2>
        ${perms.length
          ? html`<p class="muted">The draft will ask Home Assistant to read these entities.</p><ul>${perms.map((e) => html`<li class="mono">${e}</li>`)}</ul>`
          : html`<p class="muted">The draft does not ask Home Assistant for anything.</p>`}
        <p class="muted">After Apply, Home Assistant asks you to approve changes to this list.</p>
      </section>
    `;
  }

  private stat(label: string, n: number, max: number) {
    return html`<div class="card stat"><div class="v">${n}<span class="muted"> / ${max}</span></div><div class="l">${label}</div></div>`;
  }
}
