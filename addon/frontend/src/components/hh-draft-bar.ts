import { css, html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { classMap } from 'lit/directives/class-map.js';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';
import { ApiError } from '../lib/api';
import { collectIssues, type Issue } from '../lib/issues';
import { StoreElement, confirmAction } from './base';
import { ago, agoChangeIn, meters, statusChip } from './parts';

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
      .issues { position: relative; }
      .issues .pop { position: absolute; top: calc(100% + 6px); left: 0; z-index: 9; background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); box-shadow: var(--lift); padding: 8px; width: min(440px, calc(100vw - 32px)); max-height: 60vh; overflow: auto; display: grid; gap: 4px; }
      .issues .row { display: grid; grid-template-columns: auto 1fr auto; gap: 8px; align-items: start; padding: 4px; }
      .issues .row .msg { overflow-wrap: anywhere; white-space: normal; min-width: 0; }
      .issues .row .lvl.error { color: var(--bad); }
      .issues .row .lvl.warning { color: var(--ir); }
      .chip.warn { background: var(--irs); color: var(--ir); }
      .meters .meter { min-width: 92px; }
      .chipbtn { border: 0; cursor: pointer; }
      .short { display: none; }
      .actions { display: flex; gap: 8px; margin-left: auto; align-items: center; }
      .noterow { display: flex; gap: 6px; align-items: center; }
      .noterow input { height: 32px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 8px; min-width: 0; width: 22ch; }
      @media (max-width: 720px) {
        :host { position: fixed; top: auto; bottom: 0; left: 0; right: 0; padding-bottom: env(safe-area-inset-bottom, 0px); border-top: 1px solid var(--line); }
        .db { padding: 8px 16px; flex-wrap: nowrap; gap: 8px; }
        .meters, .state .muted, .long { display: none; }
        .short { display: inline; }
        .chip.ok { display: none; }
        .issues .pop { top: auto; bottom: calc(100% + 6px); }
        .state { overflow: hidden; white-space: nowrap; }
        .state { flex: 1; font-size: 13px; }
      }
    `,
  ];
  @state() private usageOpen = false;
  @state() private issuesOpen = false;
  protected override willUpdate(): void {
    const s = this.store;
    if (this.issuesOpen && !collectIssues(s.errors, s.warnings, s.checks, s.config).length) this.issuesOpen = false;
  }
  private onDocClick = (e: Event) => {
    if (this.issuesOpen && !e.composedPath().includes(this.renderRoot.querySelector('.issues') as EventTarget)) this.closeIssues(false);
  };
  private closeIssues(refocus: boolean) {
    this.issuesOpen = false;
    if (refocus) void this.updateComplete.then(() => this.renderRoot.querySelector<HTMLElement>('#issues-chip')?.focus());
  }
  override connectedCallback(): void {
    super.connectedCallback();
    document.addEventListener('click', this.onDocClick);
  }
  @state() private noteOpen = false;
  @state() private note = '';
  @state() private saving = false;
  private timer?: ReturnType<typeof setTimeout>;
  override disconnectedCallback(): void {
    clearTimeout(this.timer);
    document.removeEventListener('click', this.onDocClick);
    super.disconnectedCallback();
  }
  /** "saved 3 min ago" is redrawn by one timeout to the moment its text changes, not on a clock. */
  protected override updated(): void {
    clearTimeout(this.timer);
    const s = this.store;
    const ms = s.hasDraft ? agoChangeIn(s.savedAt) : null;
    if (ms !== null) this.timer = setTimeout(() => this.requestUpdate(), ms);
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
      title: 'Discard Draft?',
      body: 'Your unsaved changes will be deleted and the last saved revision shown again.',
      ok: 'Discard Draft',
      danger: true,
    });
    if (!ok) return;
    try {
      await this.store.discard();
    } catch (e) {
      this.store.toast(e instanceof ApiError ? e.message : 'Could not discard the draft.', 'error');
    }
  }

  private async saveRev() {
    if (this.saving) return;
    this.saving = true;
    try {
      const rev = await this.store.saveRevision(this.note.trim());
      this.noteOpen = false;
      this.note = '';
      this.store.toast(`Rev ${rev} saved.`);
    } catch (e) {
      this.store.toast(e instanceof ApiError ? e.message : 'Could not save the revision.', 'error');
    } finally {
      this.saving = false;
    }
  }

  protected override render() {
    const s = this.store;
    const n = s.diff.length;
    const reason = this.applyReason();
    const errs = s.errors.length;
    const dirty = s.dirty;
    const base = s.baseRev;
    let text: string;
    if (s.hasDraft) text = base === null ? 'Unsaved Draft · No Revision Yet' : dirty ? `Rev ${base} + Unsaved Changes${n ? ` · ${n} change${n === 1 ? '' : 's'}` : ''}` : `Rev ${base} · No Changes`;
    else text = s.revs.length ? `Rev ${s.revs[s.revs.length - 1]?.rev}` : 'Empty Config';
    const short = s.hasDraft ? (base === null ? 'Unsaved Draft' : dirty ? `Rev ${base} + Unsaved` : `Rev ${base}`) : 'Empty';
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
      ${this.issuesChip()}
      <div class="meters long">${meters(s)}</div>
      <div class="usage">
        <button class="btn sm" aria-expanded=${this.usageOpen} aria-controls="usage-pop" @click=${() => (this.usageOpen = !this.usageOpen)}>Usage</button>
        ${this.usageOpen ? html`<div class="pop" id="usage-pop" role="group" aria-label="Usage" @keydown=${(e: KeyboardEvent) => e.key === 'Escape' && (this.usageOpen = false)}>${meters(s)}</div>` : nothing}
      </div>
      <span class="sp"></span>
      <div class="actions">
        ${this.noteOpen
          ? html`<span class="noterow"><input id="rev-note" aria-label="Revision Note" placeholder="Note (optional)" maxlength="200" .value=${this.note} @input=${(e: Event) => (this.note = (e.target as HTMLInputElement).value)} @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter') void this.saveRev(); else if (e.key === 'Escape') this.noteOpen = false; }} />
              <button class="btn sm pri" id="rev-save" aria-disabled=${this.saving} @click=${() => void this.saveRev()}>Save</button>
              <button class="btn sm ghost" @click=${() => (this.noteOpen = false)}>Cancel</button></span>`
          : html`<button class="btn" id="save-rev" ?hidden=${!s.hasDraft || !dirty} title="Keep the draft as the next numbered revision" @click=${() => { this.noteOpen = true; this.updateComplete.then(() => this.renderRoot.querySelector<HTMLInputElement>('#rev-note')?.focus()); }}>Save Revision</button>`}
        <button class="btn ghost" ?hidden=${!s.hasDraft || !dirty || s.baseRev === null} @click=${() => this.discard()}>${icon('undo')}<span class="long">Discard Draft</span><span class="vh">Discard Draft</span></button>
        <button
          class="btn pri"
          aria-disabled=${reason ? 'true' : 'false'}
          title=${reason ?? 'Review the changes, then apply them to the hub'}
          @click=${() => (reason ? undefined : this.dispatchEvent(new Event('hh-review', { bubbles: true, composed: true })))}
        >
          Review &amp; Apply
        </button>
      </div>
    </div>`;
  }

  private issuesChip() {
    const s = this.store;
    const issues = collectIssues(s.errors, s.warnings, s.checks, s.config);
    const errs = issues.filter((i) => i.level === 'error').length;
    const warns = issues.length - errs;
    if (!issues.length) {
      return statusChip('ok', 'Valid', { check: true });
    }
    const plural = (n: number, w: string) => `${n} ${w}${n === 1 ? '' : 's'}`;
    const words = errs ? `${plural(errs, 'error')}${warns ? ` · ${plural(warns, 'warning')}` : ''}` : plural(warns, 'warning');
    const label = html`<span aria-hidden="true">${errs ? '✗' : '⚠'}</span> ${words}`;
    return html`<div class="issues" @keydown=${(e: KeyboardEvent) => e.key === 'Escape' && this.issuesOpen && this.closeIssues(true)}>
      <button class=${classMap({ chip: true, chipbtn: true, bad: errs > 0, warn: errs === 0 })} id="issues-chip" aria-expanded=${this.issuesOpen} aria-controls="issues-pop" aria-haspopup="dialog" @click=${() => (this.issuesOpen = !this.issuesOpen)}>${label}</button>
      ${this.issuesOpen
        ? html`<div class="pop" id="issues-pop" role="dialog" aria-label="Issues">
            ${issues.map((i) => this.issueRow(i))}
          </div>`
        : nothing}
    </div>`;
  }
  private issueRow(i: Issue) {
    return html`<div class="row"><span class="lvl ${i.level}" role="img" aria-label=${i.level}>${i.level === 'error' ? '✗' : '⚠'}</span><span class="msg">${i.text}</span>${i.target
      ? html`<button class="btn sm" aria-label=${`Show: ${i.text}`} @click=${() => { this.closeIssues(false); this.store.showIssue(i); }}>Show</button>`
      : nothing}</div>`;
  }
}
