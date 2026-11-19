import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError, type Api, type BtCodes } from '../lib/api';
import { btError, dropRowKey, hidCode, isMapped, keyProblem, keyRows, nameOfUsage, pageName, setRowKey, type KeyRow } from '../lib/bt-model';
import { shared } from '../lib/styles';

interface Pressed {
  page: number;
  usage: number;
}

/** The key map of one Bluetooth remote: every usage it can send and the Elite key it acts as (defaults filled in), plus a
 *  learn mode that listens for a press on the real remote. `value` is the config's `keys` (only your changes); `change`
 *  fires with the new map. */
@customElement('hh-bt-keys')
export class HhBtKeys extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: grid; gap: 14px; min-width: 0; }
      .learn { display: grid; gap: 10px; padding: 14px 16px; }
      .learn h3, .tbl h3 { margin: 0; font-size: 15px; }
      .learn p { margin: 0; color: var(--tx2); font-size: 13.5px; }
      .got { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; padding: 10px 12px; border-radius: var(--r2); background: var(--accs); }
      .got b { font-size: 16px; }
      .pulse { width: 12px; height: 12px; border-radius: 50%; background: var(--acc); display: inline-block; animation: hh-pulse 1.4s ease-in-out infinite; }
      @keyframes hh-pulse { 0%, 100% { opacity: 0.3; } 50% { opacity: 1; } }
      @media (prefers-reduced-motion: reduce) { .pulse { animation: none; } }
      .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
      .unm { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
      input[type="text"] { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; width: 150px; max-width: 100%; }
      input[aria-invalid="true"] { border-color: var(--bad); }
      .scroll { overflow-x: auto; }
      table { width: 100%; border-collapse: collapse; font-size: 13px; }
      th { font-size: 11px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); white-space: nowrap; }
      td { padding: 4px 8px; border-bottom: 1px solid var(--line); vertical-align: middle; }
      tr.hit td { background: var(--accs); }
      .err { color: var(--bad); font-size: 12.5px; }
      .hint { color: var(--tx3); font-size: 12.5px; margin: 0; }
    `,
  ];
  @property({ attribute: false }) codes: BtCodes | null = null;
  @property({ attribute: false }) value: Record<string, string> = {};
  /** the remote's slot in the hub, to ignore keys of other remotes while learning (null: accept any) */
  @property({ type: Number }) peer: number | null = null;
  @property({ attribute: false }) api!: Api;
  @property() hub = 'default';
  @state() private learning = false;
  @state() private pressed: Pressed | null = null;
  @state() private seen: Pressed[] = [];
  @state() private assign = '';
  @state() private err = '';
  @state() private edits: Record<string, string> = {};
  private seq = 0;
  private timer?: ReturnType<typeof setTimeout>;

  override disconnectedCallback(): void {
    void this.stopLearn();
    super.disconnectedCallback();
  }
  private emit(v: Record<string, string>) {
    this.dispatchEvent(new CustomEvent<Record<string, string>>('change', { detail: v }));
  }

  // ------------------------------------------------------------ learn mode
  private async startLearn() {
    this.err = '';
    try {
      await this.api.btListen(this.hub, true);
      this.seq = (await this.api.btEvents(this.hub, 0)).last; // skip what is already in the ring
      this.learning = true;
      this.pressed = null;
      this.timer = setTimeout(() => void this.poll(), 600);
    } catch (e) {
      this.err = btError(e);
    }
  }
  private async stopLearn() {
    clearTimeout(this.timer);
    if (!this.learning) return;
    this.learning = false;
    try {
      await this.api.btListen(this.hub, false);
    } catch {
      /* the lease lapses by itself after 30 s */
    }
  }
  private async poll() {
    if (!this.learning) return;
    try {
      const r = await this.api.btEvents(this.hub, this.seq);
      this.seq = r.last;
      for (const ev of r.events) {
        if (!ev.down || (this.peer !== null && ev.slot !== this.peer)) continue;
        this.pressed = { page: ev.page, usage: ev.usage };
        this.assign = '';
        if (!this.seen.some((s) => s.page === ev.page && s.usage === ev.usage)) this.seen = [...this.seen, { page: ev.page, usage: ev.usage }].slice(-12);
      }
      if (!r.listening) await this.api.btListen(this.hub, true);
    } catch (e) {
      this.err = btError(e);
      if (e instanceof ApiError && e.code !== 'hub_timeout') return void (this.learning = false);
    }
    this.timer = setTimeout(() => void this.poll(), 700);
  }

  // ------------------------------------------------------------ edits
  private rowKey(r: KeyRow, key: string) {
    const c = this.codes;
    if (!c) return;
    const bad = keyProblem(c, key);
    this.edits = { ...this.edits, [r.id]: key };
    if (bad) return;
    const e = { ...this.edits };
    delete e[r.id];
    this.edits = e;
    this.emit(setRowKey(this.value, r, key.trim()));
  }
  private assignPressed(p: Pressed, key: string) {
    const c = this.codes;
    if (!c) return;
    const bad = keyProblem(c, key);
    if (bad) return void (this.err = bad);
    this.err = '';
    const id = nameOfUsage(c, p.page, p.usage);
    const row = keyRows(c, this.value).find((r) => r.page === p.page && r.usage === p.usage);
    this.emit(row ? setRowKey(this.value, { id: row.id, ...(row.was !== undefined ? { was: row.was } : {}) }, key.trim()) : { ...this.value, [id]: key.trim() });
    this.assign = '';
  }
  private rowLabel(p: Pressed): string {
    const n = nameOfUsage(this.codes, p.page, p.usage);
    return n.startsWith('HID:') ? `${pageName(p.page)} ${n.slice(4)}` : n;
  }

  protected override render() {
    const c = this.codes;
    if (!c) return html`<p class="hint" role="status">Loading the key list…</p>`;
    const rows = keyRows(c, this.value);
    const p = this.pressed;
    const cur = p ? rows.find((r) => r.page === p.page && r.usage === p.usage) : undefined;
    const unmapped = this.seen.filter((s) => !isMapped(rows, s.page, s.usage));
    return html`
      <datalist id="hh-bt-keys">${c.keys.map((k) => html`<option value=${k}></option>`)}</datalist>
      <section class="card learn" aria-label="Learn a button">
        <h3>Press a button on the remote</h3>
        <p>${this.learning ? 'Listening. Press any button on the remote; it appears here.' : 'Not sure which usage a button sends? Start listening, press it, then choose its key.'}</p>
        <div class="row">
          ${this.learning ? html`<button class="btn" @click=${() => void this.stopLearn()}>Stop listening</button><span class="pulse" aria-hidden="true"></span>` : html`<button class="btn pri" @click=${() => void this.startLearn()}>Learn a button</button>`}
        </div>
        <div role="status" aria-live="polite">
          ${p
            ? html`<div class="got"><span>You pressed</span><b>${this.rowLabel(p)}</b><span class="muted mono">${hidCode(p.page, p.usage).slice(4)}</span>
                ${cur ? html`<span>→ acts as <b>${cur.key}</b></span>` : html`<span class="chip ir">Not mapped</span>`}
                <input type="text" list="hh-bt-keys" aria-label="Key for ${this.rowLabel(p)}" placeholder="key, e.g. menu" .value=${this.assign} @input=${(e: Event) => (this.assign = (e.target as HTMLInputElement).value)} @keydown=${(e: KeyboardEvent) => e.key === 'Enter' && this.assignPressed(p, this.assign)} />
                <button class="btn sm pri" @click=${() => this.assignPressed(p, this.assign)}>${cur ? 'Change' : 'Assign'}</button></div>`
            : nothing}
        </div>
        ${this.err ? html`<span class="err" role="alert">${this.err}</span>` : nothing}
        ${unmapped.length ? html`<div class="unm"><span class="muted">Pressed but not mapped:</span>${unmapped.map((u) => html`<button class="btn sm" @click=${() => { this.pressed = u; this.assign = ''; }}>${this.rowLabel(u)}</button>`)}</div>` : nothing}
      </section>
      <section class="tbl" aria-label="Button map">
        <h3>What each button does</h3>
        <p class="hint">Defaults are filled in. Change a key to override it; a button that is not listed does nothing.</p>
        <div class="scroll"><table aria-label="Remote buttons and their keys">
          <thead><tr><th>Button</th><th>Usage</th><th>Acts as key</th><th></th></tr></thead>
          <tbody>${rows.map((r) => {
            const val = this.edits[r.id] ?? r.key;
            const bad = r.id in this.edits ? keyProblem(c, val) : null;
            return html`<tr class=${p && r.page === p.page && r.usage === p.usage ? 'hit' : ''}>
              <td><b>${r.id.startsWith('HID:') ? 'Your own code' : r.id}</b></td>
              <td class="mono">${r.page ? hidCode(r.page, r.usage).slice(4) : r.id}</td>
              <td><input type="text" list="hh-bt-keys" aria-label="Key for ${r.id}" aria-invalid=${bad ? 'true' : 'false'} .value=${val} @input=${(e: Event) => this.rowKey(r, (e.target as HTMLInputElement).value)} />${bad ? html`<div class="err" role="alert">${bad}</div>` : nothing}</td>
              <td>${r.was === undefined ? html`<button class="btn sm" aria-label="Remove ${r.id}" @click=${() => this.emit(dropRowKey(this.value, r.id))}>Remove</button>` : r.mine ? html`<button class="btn sm" aria-label="Reset ${r.id} to ${r.was}" @click=${() => { const e = { ...this.edits }; delete e[r.id]; this.edits = e; this.emit(dropRowKey(this.value, r.id)); }}>Reset</button>` : html`<span class="chip">Default</span>`}</td></tr>`;
          })}</tbody></table></div>
      </section>`;
  }
}
