import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { ApiError, type ApplyStatus } from '../lib/api';
import { diffConfig, type DiffRow } from '../lib/diff';
import type { Config } from '../lib/model';
import { linkInfo } from '../lib/hubs';
import { shared } from '../lib/styles';
import { StoreElement, confirmAction } from '../components/base';
import { when } from '../components/parts';

/** "5 min ago", "yesterday"; the exact time goes in the title. */
export function ago(sec: number, now = Date.now() / 1000): string {
  const d = Math.max(0, now - sec);
  if (d < 60) return 'just now';
  if (d < 3600) return `${Math.floor(d / 60)} min ago`;
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`;
  if (d < 172800) return 'yesterday';
  return `${Math.floor(d / 86400)} days ago`;
}

/** One line for a diff: "Device TV changed, +2 more". */
export function summarize(rows: DiffRow[]): string {
  const r = rows[0];
  if (!r) return 'No changes to the config';
  const first = `${r.area} ${r.label} ${r.kind}`;
  return rows.length > 1 ? `${first}, +${rows.length - 1} more` : first;
}

@customElement('hh-history')
export class HhHistory extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      header.top { display: grid; gap: 4px; }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; grid-template-columns: max-content max-content minmax(0, 1fr) max-content 212px; }
      li { grid-column: 1 / -1; display: grid; grid-template-columns: subgrid; column-gap: 16px; row-gap: 8px; align-items: center; padding: 10px 12px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); }
      .rev { font-weight: 650; }
      .when { color: var(--tx2); white-space: nowrap; }
      .sum { min-width: 0; overflow-wrap: anywhere; color: var(--tx2); }
      .state { display: flex; gap: 6px; flex-wrap: wrap; min-width: 0; }
      .acts { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; justify-items: stretch; }
      .acts .btn { width: 100%; }
      .acts .chip { grid-column: 1 / -1; justify-self: end; }
      .diffrow { grid-column: 1 / -1; display: grid; gap: 4px; }
      .diffrow div { display: flex; gap: 8px; flex-wrap: wrap; padding: 4px 8px; border-radius: var(--r1); background: var(--sf2); }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      @media (max-width: 720px) {
        ul { grid-template-columns: max-content minmax(0, 1fr); }
        li { grid-template-columns: subgrid; }
        .sum, .state, .acts { grid-column: 1 / -1; }
        .when { text-align: right; }
        .state:empty { display: none; }
        .acts .chip { justify-self: start; }
      }
    `,
  ];

  /** rev -> what changed from that version to the applied one (loaded on demand) */
  @state() private open: Record<number, DiffRow[] | 'loading'> = {};
  /** rev -> one line on what that revision changed compared with the one before it */
  @state() private sums: Record<number, string> = {};
  /** whether Home Assistant has taken the current version */
  @state() private link: ApplyStatus | null = null;
  private sumKey = '';

  override connectedCallback(): void {
    super.connectedCallback();
    void this.loadSummaries();
    this.store.api.applyStatus(this.store.hub).then((l) => (this.link = l), () => undefined);
  }
  protected override updated(): void {
    void this.loadSummaries();
  }

  private async loadSummaries() {
    const s = this.store;
    const key = [s.applied?.hash, ...s.history.map((h) => h.hash)].join(',');
    if (!s.applied || key === this.sumKey) return;
    this.sumKey = key;
    try {
      const cfgs: (Config | null)[] = [s.appliedConfig, ...(await Promise.all(s.history.map((h) => s.api.historyEntry(s.hub, h.rev).then((e) => e.config))))];
      const revs = [s.applied.rev, ...s.history.map((h) => h.rev)];
      const sums: Record<number, string> = {};
      revs.forEach((rev, i) => {
        sums[rev] = i + 1 < cfgs.length ? summarize(diffConfig(cfgs[i + 1] ?? null, cfgs[i] ?? null)) : rev === 1 ? 'First version' : 'Oldest version kept';
      });
      this.sums = sums;
    } catch {
      this.sumKey = ''; // retry on the next update
    }
  }

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
        title: `Restore rev ${rev}?`,
        body: 'You have unapplied changes. Restoring replaces them with that version.',
        ok: 'Replace draft',
        danger: true,
      });
      if (!ok) return;
    }
    try {
      await s.restoreAsDraft(rev);
      s.toast(`Rev ${rev} restored. Review it, then apply.`);
      location.hash = '#/overview';
    } catch (e) {
      s.toast(e instanceof ApiError ? e.message : 'Could not restore that version.', 'error');
    }
  }

  private diffRows(rev: number) {
    const d = this.open[rev];
    if (d === undefined) return nothing;
    if (d === 'loading') return html`<span class="muted diffrow">Loading…</span>`;
    return html`<div class="diffrow" role="group" aria-label="Changes from rev ${rev} to the current version">
      ${d.length
        ? d.map((r) => html`<div><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</div>`)
        : html`<span class="muted">Same as the current version.</span>`}
    </div>`;
  }

  private cells(rev: number, at: number) {
    return html`<span class="rev">Rev ${rev}</span>
      <time class="when" datetime=${new Date(at * 1000).toISOString()} title=${when(at)}>${ago(at)}</time>
      <span class="sum">${this.sums[rev] ?? ''}</span>`;
  }

  /** Applied, then whether Home Assistant has taken it: only the current version has a known state. */
  private currentState() {
    const l = this.link;
    const info = linkInfo(l, this.store.hub);
    const out = [html`<span class="chip acc">Applied</span>`];
    if (l && l.applied_rev !== null && l.acked_rev === l.applied_rev)
      out.push(l.result === 'error' ? html`<span class="chip bad">Rejected by Home Assistant</span>` : html`<span class="chip ok">Acknowledged by Home Assistant</span>`);
    else if (l && l.applied_rev !== null && info.kind !== 'unknown') out.push(html`<span class="chip ir">Waiting for Home Assistant</span>`);
    return out;
  }

  protected override render() {
    const s = this.store;
    const cur = s.applied;
    return html`
      <header class="top">
        <h1>History</h1>
        <p class="muted" id="hint">The add-on keeps the applied config and the last ${s.historyMax} before it. Restore loads an earlier version as your draft: nothing reaches the hub until you press Apply, and one Undo brings your previous draft back.</p>
      </header>
      ${!cur
        ? html`<div class="card"><p class="muted">Nothing has been applied yet, so there is no history.</p></div>`
        : html`<ul aria-label="Applied versions">
            <li>
              ${this.cells(cur.rev, cur.at)}
              <span class="state">${this.currentState()}</span>
              <span class="acts"><span class="chip ok">Current</span></span>
            </li>
            ${s.history.map(
              (h) => html`<li>
                ${this.cells(h.rev, h.at)}
                <span class="state"></span>
                <span class="acts">
                  <button class="btn sm" aria-label="Compare rev ${h.rev}" aria-expanded=${this.open[h.rev] !== undefined} title="Show what changed between rev ${h.rev} and the current version" @click=${() => void this.compare(h.rev)}>Compare</button>
                  <button class="btn sm pri" aria-label="Restore rev ${h.rev}" title="Load rev ${h.rev} as your draft. Nothing is sent to the hub until you press Apply; Undo reverses it." @click=${() => void this.restore(h.rev)}>Restore</button>
                </span>
                ${this.diffRows(h.rev)}
              </li>`,
            )}
          </ul>`}
      ${cur && s.history.length === 0 ? html`<p class="muted">This is the first applied version.</p>` : nothing}
    `;
  }
}
