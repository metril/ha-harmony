import { LitElement, css, html, nothing } from 'lit';
import { displayCommand } from '../lib/key-names';
import { customElement, property } from 'lit/decorators.js';
import { chainOrder } from '../lib/activity-edit';
import { art, deviceKind } from '../lib/device-art';
import type { Config } from '../lib/model';

/** An activity's devices left to right with the input on each wire (cards in galleries and the overview). */
@customElement('hh-chain-mini')
export class HhChainMini extends LitElement {
  static override styles = css`
    :host { display: block; }
    .mini { display: flex; align-items: center; gap: 4px; min-height: 44px; overflow-x: auto; }
    .d { display: grid; justify-items: center; width: 56px; color: var(--acc); }
    .d .art { width: 44px; height: 33px; }
    .d small { font-size: 10.5px; color: var(--tx2); max-width: 56px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .w { display: grid; justify-items: center; font-size: 10px; color: var(--tx3); max-width: 64px; }
    .w span { background: var(--sf2); border-radius: 8px; padding: 0 6px; max-width: 64px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-height: 14px; }
    .w i { display: block; width: 28px; border-top: 2px dashed var(--line); }
    .none { color: var(--tx3); font-size: 12px; }
  `;
  @property({ attribute: false }) cfg: Config = {};
  @property() activity = '';

  protected override render() {
    const order = chainOrder(this.cfg, this.activity);
    const a = this.cfg.activities?.find((x) => x.id === this.activity);
    return html`<div class="mini" role="img" aria-label="Devices in order: ${order.map((d) => this.cfg.devices?.find((x) => x.id === d)?.name ?? d).join(', then ')}">${order.map((dv, i) => {
      const d = this.cfg.devices?.find((x) => x.id === dv);
      const input = a?.devices?.find((x) => x.device === dv)?.input;
      return html`${i > 0 ? html`<span class="w"><span>${input ? displayCommand(input) : ''}</span><i></i></span>` : nothing}<span class="d">${art(d ? deviceKind(d) : 'other')}<small>${d?.name ?? dv}</small></span>`;
    })}${order.length === 0 ? html`<span class="none">No devices yet</span>` : nothing}</div>`;
  }
}
