import { LitElement, css, html } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError } from '../lib/api';
import { kindOf, validateCode } from '../lib/codes';
import type { IrAction } from '../lib/model';
import { shared } from '../lib/styles';
import { getStore } from '../lib/store';

/** The one amber control: fires the shown code once through Home Assistant (POST api/v1/ir/test). */
@customElement('hh-send-ir')
export class HhSendIr extends LitElement {
  static override styles = [
    shared,
    css`
      button { display: inline-flex; align-items: center; gap: 6px; height: 28px; padding: 0 10px; border-radius: 14px; border: 1.5px solid var(--ir); color: var(--ir); background: var(--irs); font-weight: 600; font-size: 12px; white-space: nowrap; cursor: pointer; }
      button[aria-disabled="true"] { opacity: 0.6; cursor: not-allowed; }
      :host([variant="round"]) button { width: 36px; height: 36px; padding: 0; justify-content: center; border-radius: 50%; }
      :host([variant="round"]) button .t { display: none; }
      :host([variant="round"]) button .w { font-size: 10px; }
      :host([variant="big"]) { display: block; }
      :host([variant="big"]) button { width: 180px; height: 180px; border-radius: 50%; flex-direction: column; justify-content: center; gap: 8px; font-size: 18px; border-width: 3px; margin: 0 auto; }
      :host([variant="big"]) .ic { width: 56px; height: 56px; stroke-width: 1.6; }
      @media (max-width: 720px) { button { min-height: 44px; } :host([variant="round"]) button { width: 44px; height: 44px; } }
    `,
  ];
  @property({ attribute: false }) code?: IrAction;
  /** pill (default), round (icon only, 36 px) or big (wizard power test) */
  @property({ reflect: true }) variant: 'pill' | 'round' | 'big' = 'pill';
  /** what is being sent, for the accessible name: "Send IR for vol_up" */
  @property() name = '';
  @state() private busy = false;
  @state() private sent = false;
  @state() private wait = 0;
  private timer?: ReturnType<typeof setInterval>;

  override disconnectedCallback(): void {
    clearInterval(this.timer);
    super.disconnectedCallback();
  }

  /** Why the button cannot fire right now, or null. */
  private blocked(): string | null {
    const c = this.code;
    if (!c) return 'No code to send.';
    const k = kindOf(c);
    if (k === 'flipper' || k === 'smartir') return 'Codes that point at a file cannot be test-sent. Import them into the YAML first.';
    if (!validateCode(c).ok) return 'This code is not valid yet.';
    if (this.wait > 0) return `Wait ${Math.ceil(this.wait / 1000)} s: test sends are rate limited.`;
    return null;
  }

  private cooldown(ms: number) {
    const until = Date.now() + ms;
    this.wait = ms;
    clearInterval(this.timer);
    this.timer = setInterval(() => {
      this.wait = Math.max(0, until - Date.now());
      if (this.wait === 0) clearInterval(this.timer);
    }, 250);
  }

  private async send() {
    if (this.busy || this.blocked()) return;
    const store = getStore();
    const { port_mask, ...ir } = this.code as IrAction;
    this.busy = true;
    try {
      await store.api.irTest(store.hub, ir, port_mask);
      this.sent = true;
      this.dispatchEvent(new CustomEvent('ir-sent', { bubbles: true, composed: true }));
      setTimeout(() => (this.sent = false), 1500);
    } catch (e) {
      if (e instanceof ApiError && e.status === 429) {
        this.cooldown(e.retryAfterMs ?? 1000);
      } else if (e instanceof ApiError && e.code === 'ha_unavailable') {
        store.toast('Home Assistant is not reachable from the add-on, so the code was not sent.', 'error');
      } else {
        store.toast(`Could not send the code. ${e instanceof Error ? e.message : ''}`.trim(), 'error');
      }
    } finally {
      this.busy = false;
    }
  }

  protected override render() {
    const why = this.blocked();
    const label = this.busy ? 'Sending…' : this.sent ? 'Sent' : this.wait > 0 ? `Wait ${Math.ceil(this.wait / 1000)} s` : 'Send IR';
    const what = this.name ? `Send IR for ${this.name}` : 'Send IR';
    const short = this.sent ? '✓' : this.wait > 0 ? `${Math.ceil(this.wait / 1000)}s` : '';
    return html`<button type="button" aria-disabled=${why || this.busy ? 'true' : 'false'} title=${why ?? 'Send this code once through Home Assistant'} aria-label=${why ? `${what} (unavailable: ${why})` : what} @click=${(e: Event) => { e.stopPropagation(); void this.send(); }} @keydown=${(e: KeyboardEvent) => e.stopPropagation()}>
      ${short && this.variant === 'round' ? html`<span class="w">${short}</span>` : html`<svg class="ic" viewBox="0 0 24 24" aria-hidden="true"><path d="M4 8a12 12 0 0116 0M7 12a7 7 0 0110 0M10 16a2.5 2.5 0 014 0M12 20v.01"></path></svg>`}
      <span class="t">${label}</span>
    </button>`;
  }
}
