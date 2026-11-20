import { LitElement, css, html } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { displayKey } from '../lib/key-names';
import { stockIconSvg } from '../lib/icon-shapes';
import { STOCK_ICONS } from '../lib/stock-icons';
import { shared } from '../lib/styles';

/** A searchable grid of the remote's stock icons. `value` is the icon name ('' = the default); fires `hh-icon` with the new name or undefined. */
@customElement('hh-icon-picker')
export class HhIconPicker extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; }
      input[type="search"] { height: 34px; width: 100%; box-sizing: border-box; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; margin-bottom: 8px; }
      .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(44px, 1fr)); gap: 6px; max-height: 220px; overflow: auto; padding: 2px; }
      .grid button { height: 44px; display: grid; place-items: center; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf2); color: var(--tx); cursor: pointer; font-size: 11.5px; }
      .grid button:hover { background: var(--sf3); }
      .grid button[aria-pressed="true"] { background: var(--acc); color: var(--onacc); border-color: var(--acc); }
      .grid .sic { width: 24px; height: 24px; }
      .none { margin: 8px 0 0; }
    `,
  ];
  @property() value = '';
  @state() private q = '';
  private pick(name: string | undefined): void {
    this.dispatchEvent(new CustomEvent('hh-icon', { detail: name, bubbles: true, composed: true }));
  }
  protected override render() {
    const q = this.q.trim().toLowerCase();
    const icons = STOCK_ICONS.filter((i) => !q || i.name.includes(q) || i.keywords.some((k) => k.includes(q)));
    return html`<input type="search" placeholder="Search icons" aria-label="Search icons" .value=${this.q} @input=${(e: Event) => (this.q = (e.target as HTMLInputElement).value)} />
      <div class="grid" role="group" aria-label="Icon">
        ${icons.map((i) => html`<button aria-pressed=${this.value === i.name} aria-label=${displayKey(i.name)} title=${displayKey(i.name)} @click=${() => this.pick(i.name)}>${stockIconSvg(i.id)}</button>`)}
      </div>
      ${icons.length ? '' : html`<p class="muted">No icon matches.</p>`}
      <button class="btn sm none" ?disabled=${!this.value} @click=${() => this.pick(undefined)}>Use the Default Icon</button>`;
  }
}
declare global { interface HTMLElementTagNameMap { 'hh-icon-picker': HhIconPicker } }
