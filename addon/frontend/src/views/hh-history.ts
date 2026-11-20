import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { ApiError, type ApplyStatus, type ConfigSummary, type HubsResponse } from '../lib/api';
import { diffConfig, type DiffRow } from '../lib/diff';
import { linkInfo, linkProblem } from '../lib/hubs';
import { shared } from '../lib/styles';
import { StoreElement, confirmAction } from '../components/base';
import { statusChip, when } from '../components/parts';

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
      .cur { display: flex; gap: 8px 12px; align-items: center; flex-wrap: wrap; padding: 10px 12px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); }
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
      li { display: grid; grid-template-columns: max-content max-content minmax(0, 1fr); column-gap: 16px; row-gap: 8px; align-items: center; padding: 10px 12px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); }
      .rev { font-weight: 650; }
      .when { color: var(--tx2); white-space: nowrap; }
      .note { min-width: 0; overflow-wrap: anywhere; color: var(--tx2); }
      .badges { grid-column: 1 / -1; display: flex; gap: 6px; flex-wrap: wrap; }
      .acts { grid-column: 1 / -1; display: flex; gap: 8px; flex-wrap: wrap; }
      .why { margin: 0; padding: 6px 10px; border-radius: var(--r1); background: var(--bads); color: var(--bad); overflow-wrap: anywhere; flex-basis: 100%; }
      .diffrow { grid-column: 1 / -1; display: grid; gap: 4px; }
      .diffrow div { display: flex; gap: 8px; flex-wrap: wrap; padding: 4px 8px; border-radius: var(--r1); background: var(--sf2); }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      a.btn { text-decoration: none; display: inline-flex; align-items: center; }
      @media (max-width: 720px) { li { grid-template-columns: max-content minmax(0, 1fr); } .note { grid-column: 1 / -1; } }
    `,
  ];

  /** rev -> what changed from that revision to the draft (loaded on demand) */
  @state() private open: Record<number, DiffRow[] | 'loading'> = {};
  @state() private link: ApplyStatus | null = null;
  @state() private hubs: HubsResponse | null = null;
  @state() private row: ConfigSummary | null = null;
  private unsubs: (() => void)[] = [];
  @state() private why = false;

  override connectedCallback(): void {
    super.connectedCallback();
    const live = this.store.api.live;
    this.unsubs = [
      live.sub('hubs', (v) => (this.hubs = v as HubsResponse)),
      live.sub('configs', (v) => (this.row = (v as { configs: ConfigSummary[] }).configs.find((c) => c.id === this.store.configId) ?? null)),
      live.sub(`hub:${this.store.hub}`, (v) => (this.link = v as ApplyStatus)),
    ];
    void this.store.refreshRevs();
  }
  override disconnectedCallback(): void {
    for (const u of this.unsubs) u();
    this.unsubs = [];
    super.disconnectedCallback();
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
      const h = await this.store.api.getRev(this.store.configId, rev);
      this.open = { ...this.open, [rev]: diffConfig(h.config, this.store.config) };
    } catch (e) {
      this.close(rev);
      this.store.toast(e instanceof ApiError ? e.message : 'Could not load that revision.', 'error');
    }
  }

  private async restore(rev: number) {
    const s = this.store;
    if (s.dirty) {
      const ok = await confirmAction(this, {
        title: `Restore Rev ${rev}?`,
        body: 'You have unsaved changes. Restoring replaces them with that revision. Save a revision first to keep them.',
        ok: 'Replace Draft',
        danger: true,
      });
      if (!ok) return;
    }
    try {
      await s.restoreRev(rev);
      location.hash = '#/overview';
    } catch (e) {
      s.toast(e instanceof ApiError ? e.message : 'Could not restore that revision.', 'error');
    }
  }

  private async duplicate(rev: number) {
    const s = this.store;
    try {
      const r = await s.api.createConfig({ name: `${s.configName} Rev ${rev}`, from: { id: s.configId, rev } });
      const u = new URL(location.href);
      u.searchParams.set('config', r.id);
      u.hash = '#/history';
      location.href = u.toString();
    } catch (e) {
      s.toast(e instanceof ApiError ? e.message : 'Could not duplicate that revision.', 'error');
    }
  }

  private applyTo(rev: number) {
    this.dispatchEvent(new CustomEvent('hh-review', { bubbles: true, composed: true, detail: { rev } }));
  }

  private async revertPrevious() {
    const s = this.store;
    const prev = s.history[0];
    if (!prev) return;
    const ok = await confirmAction(this, {
      title: 'Revert Hub?',
      body: `${s.hub} will run ${prev.name} Rev ${prev.rev} again.`,
      ok: 'Revert',
      danger: true,
    });
    if (!ok) return;
    try {
      await s.revertTo(s.hub);
      s.toast(`${s.hub} reverted to ${prev.name} Rev ${prev.rev}.`);
    } catch (e) {
      s.toast(e instanceof ApiError ? e.message : 'Could not revert.', 'error');
    }
  }

  private diffRows(rev: number) {
    const d = this.open[rev];
    if (d === undefined) return nothing;
    if (d === 'loading') return html`<span class="muted diffrow">Loading…</span>`;
    return html`<div class="diffrow" role="group" aria-label="Changes from rev ${rev} to the draft">
      ${d.length
        ? d.map((r) => html`<div><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</div>`)
        : html`<span class="muted">Same as the draft.</span>`}
    </div>`;
  }

  /** The selected hub's one status: is this what the remote is running? */
  private currentStatus() {
    const l = this.link;
    const hub = this.store.hub;
    if (!l || l.applied_rev === null) return nothing;
    const acked = l.acked_rev === l.applied_rev;
    if (acked && l.result === 'error')
      return html`${statusChip('bad', 'Rejected By Hub', { title: l.detail ?? undefined })}
        <button class="btn sm" aria-expanded=${this.why} aria-controls="why" @click=${() => (this.why = !this.why)}>See Why</button>`;
    if (acked) return statusChip('ok', 'On the Remote', { check: true });
    const info = linkInfo(this.hubs, l, hub);
    if (info.kind === 'none') return statusChip('', 'Not Sent', { title: linkProblem(info, hub) ?? undefined });
    return statusChip('', 'Sending…', { title: linkProblem(info, hub) ?? 'Waiting for Home Assistant to pick it up.' });
  }

  protected override render() {
    const s = this.store;
    const cur = s.applied;
    const running = this.row?.hubs ?? [];
    const revs = [...s.revs].reverse();
    return html`
      <header class="top">
        <h1>Revisions</h1>
        <p class="muted" id="hint">${s.configName}: restore loads a revision as your draft; nothing is sent until you apply it. Revisions are kept until the config is deleted.</p>
      </header>
      ${cur
        ? html`<div class="cur" id="hub-runs" role="status">
            <span><strong>${s.hub}</strong> runs ${cur.name} Rev ${cur.rev}</span>
            ${this.currentStatus()}
            ${s.history.length ? html`<button class="btn sm" id="revert-prev" title="Run ${s.history[0]?.name} Rev ${s.history[0]?.rev} on ${s.hub} again" @click=${() => void this.revertPrevious()}>Revert to Previous</button>` : nothing}
            ${this.why && this.link?.result === 'error' ? html`<p class="why" id="why">${this.link.detail ?? 'The hub rejected this config.'}</p>` : nothing}
          </div>`
        : nothing}
      ${revs.length === 0
        ? html`<div class="card"><p class="muted">No revisions yet. Use Save Revision in the bar above to keep a numbered copy of the draft.</p></div>`
        : html`<ul aria-label="Revisions">
            ${revs.map(
              (r) => html`<li>
                <span class="rev">Rev ${r.rev}</span>
                <time class="when" datetime=${new Date(r.at * 1000).toISOString()} title=${when(r.at)}>${ago(r.at)}</time>
                <span class="note">${r.note}</span>
                ${running.some((h) => h.rev === r.rev)
                  ? html`<span class="badges">${running.filter((h) => h.rev === r.rev).map((h) => statusChip('ok', `Applied On ${h.hub}`, { check: true }))}</span>`
                  : nothing}
                <span class="acts">
                  <button class="btn sm" aria-label="Compare rev ${r.rev}" aria-expanded=${this.open[r.rev] !== undefined} title="Show what changed between rev ${r.rev} and the draft" @click=${() => void this.compare(r.rev)}>Compare</button>
                  <button class="btn sm pri" aria-label="Restore rev ${r.rev} to draft" title="Load rev ${r.rev} as your draft. Nothing is sent to a hub." @click=${() => void this.restore(r.rev)}>Restore To Draft</button>
                  <button class="btn sm" aria-label="Duplicate rev ${r.rev} as new config" @click=${() => void this.duplicate(r.rev)}>Duplicate As New Config</button>
                  <button class="btn sm" aria-label="Apply rev ${r.rev} to hub" @click=${() => this.applyTo(r.rev)}>Apply To Hub</button>
                  <a class="btn sm" aria-label="Export rev ${r.rev} as YAML" href=${s.api.exportUrl(s.configId, `rev:${r.rev}`)} download="${s.configId}-rev-${r.rev}.yaml">Export YAML</a>
                </span>
                ${this.diffRows(r.rev)}
              </li>`,
            )}
          </ul>`}
    `;
  }
}
