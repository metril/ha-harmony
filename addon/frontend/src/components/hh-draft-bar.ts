import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';
import { ApiError } from '../lib/api';
import { StoreElement, confirmAction } from './base';
import { ago, meters, statusChip } from './parts';

/** Sticky draft bar: state, validation chip, meters, Discard draft, Review & apply. */
@customElement('hh-draft-bar')
export class HhDraftBar extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: block; position: sticky; top: 48px; z-index: 5; }
      .db { display: flex; align-items: center; gap: 12px 16px; flex-wrap: wrap; padding: 8px 20px; background: var(--sf); border-bottom: 1px solid var(--line); }
      .state { display: flex; align-items: center; gap: 8px; font-weight: 600; min-width: 0; }
      .state .muted { font-weight: 400; }
      .dot { width: 8px; height: 8px; border-radius: 4px; background: var(--ir); flex: none; }
      .dot.ok { background: var(--ok); }
      .dot.err { background: var(--bad); }
      .meters { display: flex; gap: 12px; flex-wrap: wrap; }
      .usage { display: none; position: relative; }
      .usage .pop { position: absolute; top: calc(100% + 6px); left: 0; z-index: 9; background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); box-shadow: var(--lift); padding: 12px; display: grid; gap: 10px; width: 220px; }
      @media (max-width: 1100px) and (min-width: 721px) {
        .db { padding: 4px 16px; flex-wrap: nowrap; }
        .db .btn { height: 30px; }
        .meters.long { display: none; }
        .usage { display: block; }
        .state .muted { display: none; }
      }
      .meters .meter { min-width: 92px; }
      .chipbtn { border: 0; cursor: pointer; }
      .short { display: none; }
      .actions { display: flex; gap: 8px; margin-left: auto; }
      @media (max-width: 720px) {
        :host { position: fixed; top: auto; bottom: 0; left: 0; right: 0; padding-bottom: env(safe-area-inset-bottom, 0px); border-top: 1px solid var(--line); }
        .db { padding: 8px 16px; flex-wrap: nowrap; gap: 8px; }
        .meters, .state .muted, .long { display: none; }
        .short { display: inline; }
        .chip.ok { display: none; }
        .state { overflow: hidden; white-space: nowrap; }
        .state { flex: 1; font-size: 13px; }
      }
    `,
  ];
  @state() private usageOpen = false;
  private timer?: ReturnType<typeof setInterval>;
  override connectedCallback(): void {
    super.connectedCallback();
    this.timer = setInterval(() => this.requestUpdate(), 5000);
  }
  override disconnectedCallback(): void {
    clearInterval(this.timer);
    super.disconnectedCallback();
  }

  private applyReason(): string | null {
    const s = this.store;
    if (s.errors.length) return `Fix ${s.errors.length} error${s.errors.length === 1 ? '' : 's'} to apply`;
    if (!s.hasDraft) return 'There is no draft to apply';
    if (s.saveState === 'conflict') return 'Resolve the conflict first';
    if (s.saveState === 'error') return 'The last change could not be saved';
    return null;
  }

  private async discard() {
    const ok = await confirmAction(this, {
      title: 'Discard draft?',
      body: 'Your unapplied changes will be deleted and the applied config shown again.',
      ok: 'Discard draft',
      danger: true,
    });
    if (!ok) return;
    try {
      await this.store.discard();
    } catch (e) {
      this.store.toast(e instanceof ApiError ? e.message : 'Could not discard the draft.', 'error');
    }
  }

  protected override render() {
    const s = this.store;
    const n = s.diff.length;
    const reason = this.applyReason();
    const errs = s.errors.length;
    let text: string;
    if (s.hasDraft) text = n === 0 ? 'Draft · same as applied' : `Draft · ${n} change${n === 1 ? '' : 's'} not applied`;
    else text = s.applied ? `No changes since rev ${s.applied.rev}` : 'Nothing applied yet';
    const short = s.hasDraft ? `${n} change${n === 1 ? '' : 's'}` : s.applied ? `Rev ${s.applied.rev}` : 'Empty';
    const saveText =
      s.saveState === 'saving' || s.saveState === 'dirty'
        ? 'saving…'
        : s.saveState === 'error'
          ? this.store.offline
          ? 'not saved · retrying'
          : 'not saved'
          : s.saveState === 'conflict'
            ? 'conflict'
            : s.hasDraft && s.savedAt
              ? `saved ${ago(s.savedAt)}`
              : '';
    return html`<div class="db" role="region" aria-label="Draft">
      <div class="state">
        <span class=${classMap({ dot: true, ok: !s.hasDraft && !errs, err: errs > 0 })}></span>
        <span class="long">${text}</span><span class="short">${short}</span>
        <span class="muted" role="status" aria-live="polite">${saveText}</span>
      </div>
      ${errs
        ? html`<button class="chip bad chipbtn" @click=${() => this.showFirst()} aria-label="${errs} errors. Show the first one">${icon('alert')} ${errs} error${errs === 1 ? '' : 's'} · Show</button>`
        : statusChip('ok', 'Valid', { check: true })}
      <div class="meters long">${meters(s)}</div>
      <div class="usage">
        <button class="btn sm" aria-expanded=${this.usageOpen} aria-controls="usage-pop" @click=${() => (this.usageOpen = !this.usageOpen)}>Usage</button>
        ${this.usageOpen ? html`<div class="pop" id="usage-pop" role="group" aria-label="Usage" @keydown=${(e: KeyboardEvent) => e.key === 'Escape' && (this.usageOpen = false)}>${meters(s)}</div>` : nothing}
      </div>
      <span class="sp"></span>
      <div class="actions">
        <button class="btn ghost" ?hidden=${!s.hasDraft} @click=${() => this.discard()}>${icon('undo')}<span class="long">Discard draft</span><span class="vh">Discard draft</span></button>
        <button
          class="btn pri"
          aria-disabled=${reason ? 'true' : 'false'}
          title=${reason ?? 'Review the changes, then apply them to the hub'}
          @click=${() => (reason ? undefined : this.dispatchEvent(new Event('hh-review', { bubbles: true, composed: true })))}
        >
          Review &amp; apply
        </button>
      </div>
    </div>`;
  }

  private showFirst() {
    const f = this.store.errorSummary.first;
    if (!f) return;
    location.hash = '#/' + f.route;
    requestAnimationFrame(() => document.querySelector('hh-app')?.shadowRoot?.querySelector('main')?.focus());
  }
}
