import { css, html, nothing } from 'lit';
import { customElement } from 'lit/decorators.js';
import { ApiError } from '../lib/api';
import { shared } from '../lib/styles';
import { StoreElement, confirmAction } from '../components/base';
import { when } from '../components/parts';

@customElement('hh-history')
export class HhHistory extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
      li { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; padding: 10px 12px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); }
      .rev { font-weight: 650; min-width: 64px; }
      .hash { font-family: var(--mono); font-size: 12px; color: var(--tx3); }
    `,
  ];

  private async revert(rev: number) {
    const ok = await confirmAction(this, {
      title: `Revert to rev ${rev}?`,
      body: 'This applies that version again and discards the current draft. The hub gets the old config on the next push.',
      ok: `Revert to rev ${rev}`,
      danger: true,
    });
    if (!ok) return;
    try {
      await this.store.revertTo('previous');
      this.store.toast(`Reverted to rev ${rev}.`);
    } catch (e) {
      this.store.toast(e instanceof ApiError ? e.message : 'Could not revert.', 'error');
    }
  }

  protected override render() {
    const s = this.store;
    const cur = s.applied;
    return html`
      <h1>History</h1>
      <p class="muted">The add-on keeps the applied config and the last ${s.historyMax} before it.</p>
      ${!cur
        ? html`<div class="card"><p class="muted">Nothing has been applied yet, so there is no history.</p></div>`
        : html`<ul aria-label="Applied versions">
            <li>
              <span class="rev">Rev ${cur.rev}</span><span class="chip ok">Current</span>
              <span>${when(cur.at)}</span><span class="hash">${cur.hash.slice(0, 12)}</span>
            </li>
            ${s.history.map(
              (h, i) => html`<li>
                <span class="rev">Rev ${h.rev}</span><span>${when(h.at)}</span><span class="hash">${h.hash.slice(0, 12)}</span><span class="sp"></span>
                ${h.config
                  ? html`<button class="btn sm" @click=${() => void s.restoreAsDraft(h.config ?? {})}>Restore as draft</button>`
                  : html`<button class="btn sm" aria-disabled="true" title="The add-on does not return old configs yet">Restore as draft</button>`}
                ${i === 0
                  ? html`<button class="btn sm danger" @click=${() => void this.revert(h.rev)}>Revert to rev ${h.rev}</button>`
                  : html`<button class="btn sm" aria-disabled="true" title="Only the newest earlier version can be reverted to">Revert</button>`}
              </li>`,
            )}
          </ul>`}
      ${cur && s.history.length === 0 ? html`<p class="muted">This is the first applied version.</p>` : nothing}
    `;
  }
}
