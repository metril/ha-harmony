import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError, type ApplyStatus } from '../lib/api';
import { linkInfo, linkProblem } from '../lib/hubs';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';
import { StoreElement } from './base';
import './hh-dialog';
import './hh-page-diff';
import { meters } from './parts';

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
      .hash { font-family: var(--mono); font-size: 12px; color: var(--tx3); }
    `,
  ];
  @property({ type: Boolean }) open = false;
  @property({ type: Number }) pollMs = 2000;
  @property({ type: Number }) waitMs = 30000;
  @state() private phase: 'review' | 'applying' | 'done' | 'error' = 'review';
  @state() private rev = 0;
  @state() private message = '';
  @state() private ack: 'unknown' | 'waiting' | 'ok' | 'error' | 'timeout' = 'unknown';
  @state() private linked = true;
  @state() private lastStatus: ApplyStatus | null = null;
  private pollTimer?: ReturnType<typeof setTimeout>;

  override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has('open') && this.open) {
      this.phase = 'review';
      this.message = '';
      this.ack = 'unknown';
      this.linked = true;
    }
  }
  override disconnectedCallback(): void {
    clearTimeout(this.pollTimer);
    super.disconnectedCallback();
  }

  private close() {
    clearTimeout(this.pollTimer);
    this.dispatchEvent(new Event('hh-close', { bubbles: true, composed: true }));
  }

  private async apply() {
    this.phase = 'applying';
    try {
      this.rev = await this.store.applyDraft();
      this.phase = 'done';
      this.ack = 'waiting';
      void this.poll(Date.now());
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

  /**
   * Polls config/status. The integration has taken the bundle when acked_rev equals applied_rev
   * (both are bundle hashes); result says whether it worked. After waitMs we say Home Assistant has
   * not picked it up yet but keep polling quietly: the ack can still arrive.
   */
  private async poll(started: number): Promise<void> {
    if (this.phase !== 'done' || this.ack === 'ok' || this.ack === 'error') return;
    try {
      const s = await this.store.api.applyStatus(this.store.hub);
      this.linked = s.linked;
      this.lastStatus = s;
      if (s.applied_rev !== null && s.acked_rev === s.applied_rev && s.result === 'ok') {
        this.ack = 'ok';
        return;
      }
      if (s.applied_rev !== null && s.acked_rev === s.applied_rev && s.result === 'error') {
        this.ack = 'error';
        this.message = s.detail ?? 'The hub rejected the config.';
        return;
      }
    } catch {
      /* transient: try again */
    }
    if (this.ack !== 'timeout' && Date.now() - started >= this.waitMs) this.ack = 'timeout';
    this.pollTimer = setTimeout(() => void this.poll(started), this.pollMs);
  }

  private async revertPrevious() {
    try {
      await this.store.revertTo('previous');
      this.close();
    } catch (e) {
      this.message = e instanceof Error ? e.message : String(e);
    }
  }

  private steps(): Step[] {
    const w = this.ack === 'ok' ? 'done' : this.ack === 'error' ? 'todo' : 'pending';
    return [
      { label: 'Validated', state: 'done' },
      { label: `Saved as rev ${this.rev}`, state: 'done' },
      { label: this.ack === 'ok' ? 'On the remote' : this.ack === 'error' ? 'Rejected by hub' : 'Sending to the remote', state: w },
    ];
  }

  protected override render() {
    const s = this.store;
    const hub = s.hub === 'default' ? 'the hub' : s.hub;
    const diff = s.diff;
    if (this.phase === 'review' || this.phase === 'applying') {
      return html`<hh-dialog .open=${this.open} heading="Apply draft to ${hub}?" @close=${() => this.close()}>
        ${diff.length
          ? html`<ul class="diff" aria-label="Changes">
              ${diff.map((r) => html`<li><span class="k ${r.kind}">${r.kind}</span><span><strong>${r.area}</strong> ${r.label}</span>${r.detail ? html`<span class="muted">${r.detail}</span>` : nothing}</li>`)}
            </ul>`
          : html`<p class="muted">The draft is identical to the applied config.</p>`}
        <hh-page-diff scale="0.4"></hh-page-diff>
        <div class="meters">${meters(s)}</div>
        <p class="muted">Activities that are running keep going until they stop; the new config takes effect for the next one.</p>
        ${s.hash ? html`<div class="hash">Config hash ${s.hash}</div>` : nothing}
        ${this.message ? html`<div class="banner bad" role="alert">${this.message}</div>` : nothing}
        <button slot="footer" class="btn" @click=${() => this.close()}>Cancel</button>
        <button slot="footer" class="btn pri" aria-disabled=${this.phase === 'applying'} @click=${() => (this.phase === 'applying' ? undefined : void this.apply())}>
          ${this.phase === 'applying' ? 'Applying…' : 'Apply to hub'}
        </button>
      </hh-dialog>`;
    }
    if (this.phase === 'error') {
      return html`<hh-dialog .open=${this.open} heading="Could not apply" @close=${() => this.close()}>
        <div class="banner bad" role="alert">${this.message}</div>
        <button slot="footer" class="btn" @click=${() => this.close()}>Close</button>
      </hh-dialog>`;
    }
    return html`<hh-dialog .open=${this.open} heading="Rev ${this.rev} saved" @close=${() => this.close()}>
      <ul class="steps" aria-live="polite" aria-label="Progress">
        ${this.steps().map(
          (st) => html`<li class=${st.state}>${st.state === 'done' ? icon('check') : html`<span class="dot"></span>`}${st.label}${st.state === 'pending' ? html` <span class="muted">(pending)</span>` : nothing}</li>`,
        )}
      </ul>
      ${this.ack === 'waiting' ? html`<p class="muted">You can close this: the add-on has saved the config, and the rest happens in the background.</p>` : nothing}
      ${this.ack === 'timeout'
        ? html`<div class="banner ir" role="status">Rev ${this.rev} is saved but not on the remote yet.
            ${linkProblem(linkInfo(this.lastStatus, s.hub), s.hub) ??
            (this.linked
              ? 'Home Assistant has not picked it up yet. It will be sent as soon as the integration is connected.'
              : 'Home Assistant has not picked it up yet. Is the Harmony integration set up and linked to this add-on?')}</div>`
        : nothing}
      ${this.ack === 'error'
        ? html`<div class="banner bad" role="alert">${this.message} The hub keeps the old rev.</div>`
        : nothing}
      ${this.ack === 'error' && s.history.length
        ? html`<button slot="footer" class="btn danger" @click=${() => void this.revertPrevious()}>Revert to rev ${s.history[0]?.rev}</button>`
        : nothing}
      <button slot="footer" class="btn pri" @click=${() => this.close()}>${this.ack === 'ok' || this.ack === 'error' ? 'Done' : 'Keep waiting in background'}</button>
    </hh-dialog>`;
  }
}
