import { LitElement, css, html } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import { shared } from '../lib/styles';

/** Rail sections that are not built yet: a clear empty state instead of a dead link. */
@customElement('hh-placeholder')
export class HhPlaceholder extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; }
      .card { text-align: center; padding: 48px 16px; display: grid; gap: 8px; justify-items: center; }
      p { margin: 0; color: var(--tx2); max-width: 48ch; }
    `,
  ];
  @property() heading = '';
  @property() note = '';
  protected override render() {
    return html`<h1>${this.heading}</h1>
      <div class="card" role="status">
        <span class="chip">Not built yet</span>
        <p>${this.note || 'This section is not available in this version of the editor.'}</p>
        <p class="muted">Your config is unaffected. You can still import and export YAML, and apply from the draft bar.</p>
      </div>`;
  }
}
