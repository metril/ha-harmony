import { LitElement, html } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import './hh-bt-panel';
import './hh-bt-pair';
import './hh-bt-remote';

/** Route `#/bluetooth[/host|remote[/<address>]|keys/<index>]`: the panel, a pairing wizard, or one remote's key map. */
@customElement('hh-bluetooth')
export class HhBluetooth extends LitElement {
  @property({ attribute: false }) parts: string[] = [];
  protected override render() {
    const [a, b] = this.parts;
    if (a === 'host' || a === 'remote') return html`<hh-bt-pair .mode=${a} .addr=${b ?? ''}></hh-bt-pair>`;
    if (a === 'keys') return html`<hh-bt-remote .index=${Number(b) || 0}></hh-bt-remote>`;
    return html`<hh-bt-panel></hh-bt-panel>`;
  }
}
