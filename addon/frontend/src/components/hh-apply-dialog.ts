import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError, type ApplyStatus, type HubsResponse } from '../lib/api';
import { hubLabel, linkInfo, linkProblem, pickHub } from '../lib/hubs';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';
import { StoreElement } from './base';
import './hh-dialog';
import './hh-page-diff';
import { meters, themeNotice } from './parts';
import { pageOverflow } from '../lib/studio-model';

type Step = { label: string; state: 'done' | 'pending' | 'active' | 'todo' };

/** Review & apply: diff list, meters, then progress. Waiting for Home Assistant is non-blocking. */
@customElement('hh-apply-dialog')
export class HhApplyDialog extends StoreElement {
  static override styles = [
    shared,
    css`
      ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 4px; }
      .diff li { display: flex; gap: 8px; align-items: baseline; padding: 6px 8px; border-radius: var(--r1); background: var(--sf2); flex-wrap: wrap; }
      .k { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; min-width: 62px; }
      .k.added { color: var(--ok); } .k.changed { color: var(--ir); } .k.removed { color: var(--bad); }
      .meters { display: flex; gap: 16px; flex-wrap: wrap; }
      .steps li { display: flex; gap: 8px; align-items: center; color: var(--tx3); }
      .steps li.done { color: var(--tx); } .steps li.done .ic { color: var(--ok); }
      .steps li.active { color: var(--tx); font-weight: 600; }
      .steps .dot { width: 18px; height: 18px; border-radius: 9px; border: 2px solid var(--line); flex: none; }
      .steps li.pending .dot { border-style: dashed; }
      .hubpick { display: grid; gap: 4px; font-size: 12px; font-weight: 600; color: var(--tx2); }
      .hubpick select { height: 32px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 8px; font-weight: 400; }
      .hash { font-family: var(--mono); font-size: 12px; color: var(--tx3); }
    `,
  ];
  @property({ type: Boolean }) open = false;
  @property({ type: Number }) waitMs = 30000;
  /** apply this saved revision instead of the draft (null: the draft) */
  @property({ attribute: false }) rev: number | null = null;
  /** the hub being applied to (defaults to the selected hub each time the dialog opens) */
  @state() private target = '';
  @state() private prevRev: number | null = null;
  private hubUnsub?: () => void;
  @state() private phase: 'review' | 'applying' | 'done' | 'error' = 'review';
  @state() private appliedRev = 0;
  @state() private message = '';
  /** the author has confirmed applying with labels the remote will cut */
  @state() private cutOk = false;
  @state() private ack: 'unknown' | 'waiting' | 'ok' | 'error' | 'timeout' = 'unknown';
  @state() private lastStatus: ApplyStatus | null = null;
  @state() private hubs: HubsResponse | null = null;
  /** what the linked remote's firmware cannot show of the draft's colours (non-blocking; from the app's link status) */
  @property({ attribute: false }) themeMsgs: string[] = [];
  private deadline?: ReturnType<typeof setTimeout>;
  private unsubs: (() => void)[] = [];

  override connectedCallback(): void {
    super.connectedCallback();
    const live = this.store.api.live;
    this.target = this.store.hub;
    this.unsubs = [live.sub('hubs', (v) => (this.hubs = v as HubsResponse))];
    this.watchTarget();
  }
  /** Follow the `hub:<id>` topic of the target hub. */
  private watchTarget(): void {
    this.hubUnsub?.();
    this.lastStatus = null;
    this.hubUnsub = this.store.api.live.sub(`hub:${this.target}`, (v) => {
      this.lastStatus = v as ApplyStatus;
      this.check();
    });
  }
  private hubChoices(): string[] {
    const ids = pickHub(`?hub=${encodeURIComponent(this.store.hub)}`, this.hubs).choices;
    return ids.includes(this.store.hub) ? ids : [this.store.hub, ...ids];
  }

  override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has('open') && this.open) {
      this.phase = 'review';
      this.message = '';
      this.ack = 'unknown';
      if (this.target !== this.store.hub) {
        this.target = this.store.hub;
        this.watchTarget();
      }
    }
  }
  override disconnectedCallback(): void {
    clearTimeout(this.deadline);
    this.hubUnsub?.();
    for (const u of this.unsubs) u();
    this.unsubs = [];
    super.disconnectedCallback();
  }

  private close() {
    clearTimeout(this.deadline);
    this.dispatchEvent(new Event('hh-close', { bubbles: true, composed: true }));
  }

  private async apply() {
    this.phase = 'applying';
    try {
      this.appliedRev = this.rev === null ? await this.store.applyDraft(this.target) : await this.store.applyRev(this.target, this.rev);
      this.prevRev = (await this.store.api.hubState(this.target).catch(() => null))?.history[0]?.rev ?? null;
      this.phase = 'done';
      this.ack = 'waiting';
      this.check();
      // a UI deadline only (nothing is polled): after waitMs we say Home Assistant has not picked it up, the ack can still arrive
      clearTimeout(this.deadline);
      this.deadline = setTimeout(() => {
        if (this.ack === 'waiting') this.ack = 'timeout';
      }, this.waitMs);
    } catch (e) {
      if (e instanceof ApiError && e.status === 422) {
        this.close();
        const f = this.store.errorSummary.first;
        if (f) location.hash = '#/' + f.route;
        return;
      }
      this.phase = 'error';
      this.message = e instanceof Error ? e.message : String(e);
    }
  }

  /** The integration has taken the bundle when acked_rev equals applied_rev (both are bundle hashes); result says whether it worked. */
  private check(): void {
    const s = this.lastStatus;
    if (this.phase !== 'done' || (this.ack !== 'waiting' && this.ack !== 'timeout') || !s || s.applied_rev === null || s.acked_rev !== s.applied_rev) return;
    clearTimeout(this.deadline);
    if (s.result === 'ok') this.ack = 'ok';
    else if (s.result === 'error') {
      this.ack = 'error';
      this.message = s.detail ?? 'The hub rejected the config.';
    }
  }

  private async revertPrevious() {
    try {
      await this.store.revertTo(this.target);
      this.close();
    } catch (e) {
      this.message = e instanceof Error ? e.message : String(e);
    }
  }

  /** Labels the remote would cut in the draft, from the live preview (the same pixel rule as the page editor). */
  private cutLabels(): { page: string; label: string }[] {
    return (this.store.config.pages ?? []).flatMap((p, i) => {
      const pp = this.store.pages[i];
      return pageOverflow(p, pp?.ok && pp.page ? pp : null).map((o) => ({ page: p.title || p.id, label: o.label }));
    });
  }

  private steps(): Step[] {
    const w = this.ack === 'ok' ? 'done' : this.ack === 'error' ? 'todo' : 'pending';
    return [
      { label: 'Validated', state: 'done' },
      { label: `Saved as rev ${this.appliedRev}`, state: 'done' },
      { label: this.ack === 'ok' ? 'On the Remote' : this.ack === 'error' ? 'Rejected By Hub' : 'Sending to the Remote', state: w },
    ];
  }

  protected override render() {
    const s = this.store;
    const hub = this.target === 'default' ? 'the hub' : this.target;
    const diff = this.rev === null ? s.diff : [];
    const choices = this.hubChoices();
    const cut = this.rev === null ? this.cutLabels() : [];
    if (this.phase === 'review' || this.phase === 'applying') {
      return html`<hh-dialog .open=${this.open} heading=${this.rev === null ? `Apply Draft to ${hub}?` : `Apply Rev ${this.rev} to ${hub}?`} @close=${() => this.close()}>
        <label class="hubpick">Target Hub
          <select id="apply-hub" aria-label="Target Hub" ?disabled=${this.phase === 'applying'} @change=${(e: Event) => { this.target = (e.target as HTMLSelectElement).value; this.watchTarget(); }}>
            ${choices.map((id) => html`<option value=${id} ?selected=${id === this.target}>${hubLabel(this.hubs, id)}</option>`)}
          </select>
        </label>
        ${this.rev !== null ? html`<p class="muted">${s.configName} Rev ${this.rev} replaces what ${hub} runs now.</p>` : nothing}
        ${this.rev !== null ? nothing : diff.length
          ? html`<ul class="diff" aria-label="Changes">
              ${diff.map((r) => html`<li><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</li>`)}
            </ul>`
          : html`<p class="muted">The draft is identical to its base revision.</p>`}
        ${this.rev === null ? html`<hh-page-diff scale="0.4"></hh-page-diff>` : nothing}
        ${themeNotice(this.themeMsgs)}
        ${this.rev === null ? html`<div class="meters">${meters(s)}</div>` : nothing}
        <p class="muted">Activities that are running keep going until they stop; the new config takes effect for the next one.</p>
        ${this.rev === null && s.hash ? html`<div class="hash">Config hash ${s.hash}</div>` : nothing}
        ${cut.length
          ? html`<div class="banner ir" role="alert">${cut.length} label${cut.length === 1 ? '' : 's'} will be cut off on the remote: ${cut.slice(0, 3).map((c) => `${c.page}: ${c.label}`).join('; ')}${cut.length > 3 ? '…' : ''}
              <label class="chk"><input type="checkbox" .checked=${this.cutOk} @change=${(e: Event) => (this.cutOk = (e.target as HTMLInputElement).checked)} /> Apply anyway</label></div>`
          : nothing}
        ${this.message ? html`<div class="banner bad" role="alert">${this.message}</div>` : nothing}
        <button slot="footer" class="btn" @click=${() => this.close()}>Cancel</button>
        <button slot="footer" class="btn pri" aria-disabled=${this.phase === 'applying' || (cut.length > 0 && !this.cutOk)} @click=${() => (this.phase === 'applying' || (cut.length > 0 && !this.cutOk) ? undefined : void this.apply())}>
          ${this.phase === 'applying' ? 'Applying…' : 'Apply To Hub'}
        </button>
      </hh-dialog>`;
    }
    if (this.phase === 'error') {
      return html`<hh-dialog .open=${this.open} heading="Could Not Apply" @close=${() => this.close()}>
        <div class="banner bad" role="alert">${this.message}</div>
        <button slot="footer" class="btn" @click=${() => this.close()}>Close</button>
      </hh-dialog>`;
    }
    return html`<hh-dialog .open=${this.open} heading="Rev ${this.appliedRev} applied to ${hub}" @close=${() => this.close()}>
      <ul class="steps" aria-live="polite" aria-label="Progress">
        ${this.steps().map(
          (st) => html`<li class=${st.state}>${st.state === 'done' ? icon('check') : html`<span class="dot"></span>`}${st.label}${st.state === 'pending' ? html` <span class="muted">(pending)</span>` : nothing}</li>`,
        )}
      </ul>
      ${this.ack === 'waiting' ? html`<p class="muted">You can close this: the add-on has saved the config, and the rest happens in the background.</p>` : nothing}
      ${this.ack === 'timeout'
        ? html`<div class="banner ir" role="status">Rev ${this.appliedRev} is saved but not on the remote yet.
            ${linkProblem(linkInfo(this.hubs, this.lastStatus, this.target), this.target) ??
            (this.lastStatus?.connected
              ? 'Home Assistant has not picked it up yet. It will be sent as soon as the integration is connected.'
              : 'Home Assistant has not picked it up yet. Is the Harmony integration set up and linked to this add-on?')}</div>`
        : nothing}
      ${this.ack === 'error'
        ? html`<div class="banner bad" role="alert">${this.message} The hub keeps the old rev.</div>`
        : nothing}
      ${this.ack === 'error' && this.prevRev !== null
        ? html`<button slot="footer" class="btn danger" @click=${() => void this.revertPrevious()}>Revert to rev ${this.prevRev}</button>`
        : nothing}
      <button slot="footer" class="btn pri" @click=${() => this.close()}>${this.ack === 'ok' || this.ack === 'error' ? 'Done' : 'Keep Waiting in Background'}</button>
    </hh-dialog>`;
  }
}
