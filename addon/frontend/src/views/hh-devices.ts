import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import '../components/hh-send-ir';
import '../components/hh-workspace';
import { StoreElement } from '../components/base';
import { summarize } from '../lib/codes';
import { LIMITS, type Device } from '../lib/model';
import { hashFor } from '../lib/router';
import { shared } from '../lib/styles';

/**
 * Read-only device browser in the three-pane workspace. It is the seed of the visual device
 * gallery: the table-based editor was dropped when the design became visual-first.
 */
@customElement('hh-devices')
export class HhDevices extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      .head { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
      input[type="search"] { height: 36px; width: 100%; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 10px; }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 6px; }
      .dev { display: flex; gap: 8px; align-items: center; width: 100%; text-align: left; padding: 8px 10px; min-height: 44px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); text-decoration: none; }
      .dev:hover { background: var(--sf2); }
      .dev.on { background: var(--accs); border-color: var(--acc); }
      .dev .n { margin-left: auto; font-size: 12px; color: var(--tx3); }
      table { width: 100%; border-collapse: collapse; font-size: 13px; }
      th { font-size: 11px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); }
      td { padding: 6px 8px; border-bottom: 1px solid var(--line); vertical-align: middle; min-height: 40px; overflow-wrap: anywhere; }
      .scroll { overflow-x: auto; }
      dl { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; margin: 0; }
      dt { color: var(--tx3); font-size: 12px; } dd { margin: 0; overflow-wrap: anywhere; }
    `,
  ];
  @property() selected = '';
  @state() private q = '';

  protected override render() {
    const all = this.store.config.devices ?? [];
    const list = all.filter((d) => `${d.name ?? ''} ${d.id}`.toLowerCase().includes(this.q.toLowerCase()));
    const dev = all.find((d) => d.id === this.selected) ?? all[0];
    const errBySec = this.store.errorSummary.bySection['devices'] ?? 0;
    return html`
      <div class="head">
        <h1>Devices</h1>
        <span class="chip">${all.length} / ${LIMITS.devices}</span>
        ${errBySec ? html`<span class="chip bad">${errBySec} error${errBySec === 1 ? '' : 's'}</span>` : nothing}
      </div>
      <div class="banner" role="note">Device editing is being rebuilt as a visual editor. This is a read-only view of what the draft holds.</div>
      ${all.length === 0
        ? html`<div class="card"><p class="muted">No devices yet.</p></div>`
        : html`<hh-workspace>
            <div slot="palette">
              <input type="search" aria-label="Filter devices" placeholder="Filter devices" .value=${this.q} @input=${(e: Event) => (this.q = (e.target as HTMLInputElement).value)} />
              <ul>
                ${list.map(
                  (d) => html`<li><a class=${classMap({ dev: true, on: d.id === dev?.id })} href=${hashFor('devices', d.id)} aria-current=${d.id === dev?.id ? 'true' : 'false'}>
                    <span>${d.name ?? d.id}</span><span class="n">${Object.keys(d.commands ?? {}).length}</span></a></li>`,
                )}
              </ul>
            </div>
            <div slot="canvas">${dev ? this.commands(dev) : nothing}</div>
            <div slot="inspector">${dev ? this.details(dev) : nothing}</div>
          </hh-workspace>`}
    `;
  }

  private commands(d: Device) {
    const rows = Object.entries(d.commands ?? {});
    return html`<section class="card" aria-label="Commands of ${d.name ?? d.id}">
      <h2>${d.name ?? d.id} <span class="muted">${rows.length} commands</span></h2>
      <div class="scroll">
        <table>
          <thead><tr><th>Name</th><th>Code</th><th>Ports</th><th></th></tr></thead>
          <tbody>
            ${rows.map(([name, a]) => html`<tr><td>${name}</td><td class="mono">${summarize(a)}</td><td class="mono">${a.port_mask ?? 'all'}</td><td><hh-send-ir></hh-send-ir></td></tr>`)}
          </tbody>
        </table>
      </div>
    </section>`;
  }

  private details(d: Device) {
    const p = d.power;
    const power = p?.toggle ? `Toggle · ${summarize(p.toggle)}` : p?.on || p?.off ? `On ${summarize(p.on)} · Off ${summarize(p.off)}` : 'None';
    return html`<section class="card" aria-label="Device details">
      <h2>Details</h2>
      <dl>
        <dt>Id</dt><dd class="mono">${d.id}</dd>
        <dt>Power</dt><dd>${power}</dd>
        <dt>Settle</dt><dd>${d.settle_ms ?? 0} ms</dd>
        <dt>Inputs</dt><dd>${Object.keys(d.inputs ?? {}).join(', ') || 'none'}</dd>
        ${d.aliases?.length ? html`<dt>Aliases</dt><dd>${d.aliases.join(', ')}</dd>` : nothing}
        ${d.source ? html`<dt>Source</dt><dd>From IR archive · ${String(d.source.mfr ?? '')} ${String(d.source.model_file ?? '')}</dd>` : nothing}
      </dl>
    </section>`;
  }
}
