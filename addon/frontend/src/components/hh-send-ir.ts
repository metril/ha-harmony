import { LitElement, css, html } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import { shared } from '../lib/styles';

/** The one amber control. Disabled (with a reason) until POST api/v1/ir/test exists. */
@customElement('hh-send-ir')
export class HhSendIr extends LitElement {
  static override styles = [
    shared,
    css`
      button { display: inline-flex; align-items: center; gap: 6px; height: 28px; padding: 0 10px; border-radius: 14px; border: 1.5px solid var(--ir); color: var(--ir); background: var(--irs); font-weight: 600; font-size: 12px; white-space: nowrap; cursor: pointer; }
      button[aria-disabled="true"] { opacity: 0.6; cursor: not-allowed; }
      @media (max-width: 720px) { button { min-height: 44px; } }
    `,
  ];
  @property() reason = 'Sending a code from the editor is not available yet.';
  protected override render() {
    return html`<button type="button" aria-disabled="true" title=${this.reason} aria-label="Send IR (unavailable)" @click=${(e: Event) => e.preventDefault()}>
      <svg class="ic" viewBox="0 0 24 24" aria-hidden="true"><path d="M4 8a12 12 0 0116 0M7 12a7 7 0 0110 0M10 16a2.5 2.5 0 014 0M12 20v.01"></path></svg>
      Send IR
    </button>`;
  }
}
