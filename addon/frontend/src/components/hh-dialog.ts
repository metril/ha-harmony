import { LitElement, css, html } from 'lit';
import { customElement, property, query } from 'lit/decorators.js';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';

/** Modal dialog on the native <dialog>: focus trap and Esc come from the platform. */
@customElement('hh-dialog')
export class HhDialog extends LitElement {
  static override styles = [
    shared,
    css`
      dialog { border: 1px solid var(--line); border-radius: var(--r3); background: var(--sf); color: var(--tx); padding: 0; width: min(560px, calc(100vw - 32px)); max-height: calc(100vh - 32px); box-shadow: var(--sh); }
      dialog::backdrop { background: var(--scrim); }
      .wrap { display: flex; flex-direction: column; max-height: calc(100vh - 34px); }
      header { display: flex; align-items: center; gap: 8px; padding: 16px 20px 8px; }
      header h2 { margin: 0; font-size: 16px; font-weight: 650; flex: 1; }
      .body { padding: 8px 20px 16px; overflow: auto; display: grid; gap: 12px; }
      footer { display: flex; justify-content: flex-end; gap: 8px; padding: 12px 20px 16px; border-top: 1px solid var(--line); flex-wrap: wrap; }
    `,
  ];
  @property({ type: Boolean, reflect: true }) open = false;
  @property() heading = '';
  @query('dialog') private dlg?: HTMLDialogElement;

  protected override updated(): void {
    const d = this.dlg;
    if (!d) return;
    if (this.open && !d.open) {
      if (typeof d.showModal === 'function') d.showModal();
      else d.setAttribute('open', '');
    } else if (!this.open && d.open) {
      d.close?.();
    }
  }
  private onClose = () => {
    if (this.open) {
      this.open = false;
      this.dispatchEvent(new Event('close'));
    }
  };
  protected override render() {
    return html`<dialog aria-labelledby="t" @close=${this.onClose}>
      <div class="wrap">
        <header>
          <h2 id="t">${this.heading}</h2>
          <button class="btn ghost icon sm" aria-label="Close" @click=${() => this.dlg?.close()}>${icon('x')}</button>
        </header>
        <div class="body"><slot></slot></div>
        <footer><slot name="footer"></slot></footer>
      </div>
    </dialog>`;
  }
}
