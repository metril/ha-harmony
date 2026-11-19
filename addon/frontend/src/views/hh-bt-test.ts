import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { Api, BtCodes } from '../lib/api';
import { btError, resolveId } from '../lib/bt-model';
import { shared } from '../lib/styles';

/** Test-fire: pick a command and press it once on a paired host. `commands` maps a command name to its built-in name or HID code. */
@customElement('hh-bt-test')
export class HhBtTest extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: grid; gap: 4px; min-width: 0; }
      .r { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }
      select { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; max-width: 190px; }
      .m { font-size: 12.5px; color: var(--tx2); } .m.bad { color: var(--bad); }
    `,
  ];
  @property({ attribute: false }) api!: Api;
  @property() hub = 'default';
  @property({ type: Number }) slot: number | null = null;
  @property({ attribute: false }) codes: BtCodes | null = null;
  @property({ attribute: false }) commands: Record<string, string> = {};
  @property() label = 'device';
  @property({ type: Boolean }) disabled = false;
  @state() private pick = '';
  @state() private busy = false;
  @state() private msg: { ok: boolean; text: string } | null = null;

  private async send() {
    const name = this.pick || Object.keys(this.commands)[0];
    const code = name && this.codes ? resolveId(this.codes, this.commands[name] as string) : null;
    if (!name || !code || this.slot === null) return;
    this.busy = true;
    this.msg = null;
    try {
      await this.api.btKey(this.hub, this.slot, code.page, code.usage, 'press');
      this.msg = { ok: true, text: `Sent ${name}.` };
    } catch (e) {
      this.msg = { ok: false, text: btError(e) };
    } finally {
      this.busy = false;
    }
  }
  protected override render() {
    const names = Object.keys(this.commands);
    const off = this.disabled || this.slot === null || !names.length || !this.codes;
    const cur = this.pick && names.includes(this.pick) ? this.pick : (names[0] ?? '');
    return html`<div class="r">
        <select aria-label="Command to send to ${this.label}" .value=${cur} ?disabled=${off} @change=${(e: Event) => (this.pick = (e.target as HTMLSelectElement).value)}>${names.map((n) => html`<option value=${n} ?selected=${n === cur}>${n}</option>`)}</select>
        <button class="btn sm" ?disabled=${off || this.busy} @click=${() => void this.send()}>${this.busy ? 'Sending…' : 'Send'}</button>
      </div>
      <span class="m ${this.msg && !this.msg.ok ? 'bad' : ''}" role="status" aria-live="polite">${this.msg?.text ?? nothing}</span>`;
  }
}
