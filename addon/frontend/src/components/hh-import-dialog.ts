import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError } from '../lib/api';
import { shared } from '../lib/styles';
import { StoreElement, confirmAction } from './base';
import './hh-dialog';

/** Import YAML: pick a file or paste text. Asks before replacing an existing draft. */
@customElement('hh-import-dialog')
export class HhImportDialog extends StoreElement {
  static override styles = [
    shared,
    css`
      textarea { width: 100%; min-height: 160px; resize: vertical; font-family: var(--mono); font-size: 12.5px; background: var(--sf); color: var(--tx); border: 1px solid var(--line); border-radius: var(--r1); padding: 8px; }
      input[type="file"] { max-width: 100%; }
      ul { margin: 4px 0 0; padding-left: 18px; }
      label { font-size: 12px; font-weight: 600; color: var(--tx2); display: block; margin-bottom: 4px; }
    `,
  ];
  @property({ type: Boolean }) open = false;
  @state() private text = '';
  @state() private busy = false;
  @state() private problem: { msg: string; items: string[] } | null = null;

  override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has('open') && this.open) {
      this.text = '';
      this.problem = null;
    }
  }

  private async pick(e: Event) {
    const f = (e.target as HTMLInputElement).files?.[0];
    if (f) this.text = await f.text();
  }

  private async submit() {
    if (!this.text.trim()) {
      this.problem = { msg: 'Choose a YAML file or paste its contents first.', items: [] };
      return;
    }
    if (this.store.hasDraft) {
      const ok = await confirmAction(this, {
        title: 'Replace Draft?',
        body: 'You have unapplied changes. Importing replaces them with the YAML.',
        ok: 'Replace Draft',
        danger: true,
      });
      if (!ok) return;
    }
    this.busy = true;
    this.problem = null;
    try {
      await this.store.importYaml(this.text);
      this.close();
      this.store.toast('YAML imported as a draft. Review it, then apply.');
    } catch (e) {
      if (e instanceof ApiError) {
        if (e.code === 'ir_files_unresolvable')
          this.problem = { msg: 'The YAML points to IR files the add-on cannot read. Put the codes in the YAML itself, then import again.', items: e.files };
        else if (e.code === 'invalid_config')
          this.problem = { msg: 'The YAML is not a valid config.', items: e.errors.map((x) => `${x.path.join('.') || 'config'}: ${x.msg}`) };
        else this.problem = { msg: e.message, items: [] };
      } else this.problem = { msg: String(e), items: [] };
    } finally {
      this.busy = false;
    }
  }

  private close() {
    this.dispatchEvent(new Event('hh-close', { bubbles: true, composed: true }));
  }

  protected override render() {
    const p = this.problem;
    return html`<hh-dialog .open=${this.open} heading="Import YAML" @close=${() => this.close()}>
      <div>
        <label for="file">YAML File</label>
        <input id="file" type="file" accept=".yaml,.yml,text/yaml" @change=${(e: Event) => void this.pick(e)} />
      </div>
      <div>
        <label for="txt">Or Paste YAML</label>
        <textarea id="txt" spellcheck="false" .value=${this.text} @input=${(e: Event) => (this.text = (e.target as HTMLTextAreaElement).value)}></textarea>
      </div>
      ${p
        ? html`<div class="banner bad" role="alert"><div>${p.msg}${p.items.length ? html`<ul>${p.items.map((i) => html`<li class="mono">${i}</li>`)}</ul>` : nothing}</div></div>`
        : nothing}
      <button slot="footer" class="btn" @click=${() => this.close()}>Cancel</button>
      <button slot="footer" class="btn pri" aria-disabled=${this.busy} @click=${() => (this.busy ? undefined : void this.submit())}>${this.busy ? 'Importing…' : 'Import As Draft'}</button>
    </hh-dialog>`;
  }
}
