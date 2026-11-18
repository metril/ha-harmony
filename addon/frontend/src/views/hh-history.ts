import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { ApiError } from '../lib/api';
import { diffConfig, type DiffRow } from '../lib/diff';
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
      .diffrow { flex-basis: 100%; display: grid; gap: 4px; }
      .diffrow div { display: flex; gap: 8px; flex-wrap: wrap; padding: 4px 8px; border-radius: var(--r1); background: var(--sf2); }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      .hash { font-family: var(--mono); font-size: 12px; color: var(--tx3); }
    `,
  ];

  /** rev -> what changed from that version to the applied one (loaded on demand) */
  @state() private open: Record<number, DiffRow[] | 'loading'> = {};

  private close(rev: number) {
    this.open = Object.fromEntries(Object.entries(this.open).filter(([k]) => Number(k) !== rev));
  }

  private async compare(rev: number) {
    if (this.open[rev]) {
      this.close(rev);
      return;
    }
    this.open = { ...this.open, [rev]: 'loading' };
    try {
      const h = await this.store.api.historyEntry(this.store.hub, rev);
      // "applied" is what the old version would become if restored: show old -> applied
      this.open = { ...this.open, [rev]: diffConfig(h.config, this.store.appliedConfig) };
    } catch (e) {
      this.close(rev);
      this.store.toast(e instanceof ApiError ? e.message : 'Could not load that version.', 'error');
    }
  }

  private async restore(rev: number) {
    const s = this.store;
    if (s.hasDraft) {
      const ok = await confirmAction(this, {
        title: `Restore rev ${rev} as draft?`,
        body: 'You have unapplied changes. Restoring replaces them with that version.',
        ok: 'Replace draft',
        danger: true,
      });
      if (!ok) return;
    }
    try {
      await s.restoreAsDraft(rev);
      s.toast(`Rev ${rev} restored as a draft. Review it, then apply.`);
      location.hash = '#/overview';
    } catch (e) {
      s.toast(e instanceof ApiError ? e.message : 'Could not restore that version.', 'error');
    }
  }

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

  private diffRows(rev: number) {
    const d = this.open[rev];
    if (d === undefined) return nothing;
    if (d === 'loading') return html`<span class="muted diffrow">Loading…</span>`;
    return html`<div class="diffrow" aria-label="Changes from rev ${rev} to the current version">
      ${d.length
        ? d.map((r) => html`<div><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</div>`)
        : html`<span class="muted">Same as the current version.</span>`}
    </div>`;
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
                <button class="btn sm" aria-expanded=${this.open[h.rev] !== undefined} @click=${() => void this.compare(h.rev)}>Compare</button>
                <button class="btn sm" @click=${() => void this.restore(h.rev)}>Restore as draft</button>
                ${i === 0
                  ? html`<button class="btn sm danger" @click=${() => void this.revert(h.rev)}>Revert to rev ${h.rev}</button>`
                  : html`<button class="btn sm" aria-disabled="true" title="Only the newest earlier version can be reverted to">Revert</button>`}
                ${this.diffRows(h.rev)}
              </li>`,
            )}
          </ul>`}
      ${cur && s.history.length === 0 ? html`<p class="muted">This is the first applied version.</p>` : nothing}
    `;
  }
}
