import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError, type Api, type BtCodes } from '../lib/api';
import { btError, dropRowKey, hidCode, isMapped, keyOptions, keyRows, keyText, nameOfUsage, pageName, setRowKey, type KeyMap, type KeyRow } from '../lib/bt-model';
import { shared } from '../lib/styles';

interface Pressed {
  page: number;
  usage: number;
}
const NONE = '__none__';

/** The key map of one Bluetooth remote: every usage it can send and the Elite key it acts as (defaults filled in, any default
 *  can be set to "Does nothing"), plus a learn mode that listens for a press on the real remote. `value` is the config's `keys`
 *  (only your changes); `change` fires with the new map. */
@customElement('hh-bt-keys')
export class HhBtKeys extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: grid; grid-template-columns: minmax(0, 1fr); gap: 14px; min-width: 0; }
      .learn { display: grid; grid-template-columns: minmax(0, 1fr); gap: 10px; padding: 14px 16px; min-width: 0; }
      .learn h3, .tbl h3 { margin: 0; font-size: 15px; }
      .learn p { margin: 0; color: var(--tx2); font-size: 13.5px; }
      .got { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; padding: 10px 12px; border-radius: var(--r2); background: var(--accs); min-width: 0; }
      .got b { font-size: 16px; }
      .pulse { width: 12px; height: 12px; border-radius: 50%; background: var(--acc); display: inline-block; animation: hh-pulse 1.4s ease-in-out infinite; }
      @keyframes hh-pulse { 0%, 100% { opacity: 0.3; } 50% { opacity: 1; } }
      @media (prefers-reduced-motion: reduce) { .pulse { animation: none; } }
      .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
      .unm { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
      select { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; width: 100%; max-width: 230px; }
      .got select { width: 200px; }
      .head, .kr { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(0, 0.7fr) minmax(0, 1.5fr) 84px; gap: 8px; align-items: center; padding: 4px 8px; }
      .head { font-size: 11px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); border-bottom: 1px solid var(--line); padding-block: 6px; }
      .kr { border-bottom: 1px solid var(--line); min-height: 46px; }
      .kr.hit { background: var(--accs); }
      .kr b { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .kr .end { justify-self: end; }
      .hint { color: var(--tx3); font-size: 12.5px; margin: 0; }
      .err { color: var(--bad); font-size: 12.5px; }
      @media (max-width: 640px) {
        .head { display: none; }
        .kr { grid-template-columns: minmax(0, 1fr) auto; row-gap: 4px; padding-block: 8px; }
        .kr .use { order: 2; justify-self: end; }
        .kr .pick { grid-column: 1 / -1; order: 3; }
        .kr .end { order: 2; }
        select { max-width: none; }
        .got select { width: 100%; }
      }
    `,
  ];
  @property({ attribute: false }) codes: BtCodes | null = null;
  @property({ attribute: false }) value: KeyMap = {};
  /** the remote's slot in the hub, to ignore keys of other remotes while learning (null: accept any) */
  @property({ type: Number }) peer: number | null = null;
  @property({ attribute: false }) api!: Api;
  @property() hub = 'default';
  @state() private learning = false;
  @state() private pressed: Pressed | null = null;
  @state() private seen: Pressed[] = [];
  @state() private assign = '';
  @state() private err = '';
  private seq = 0;
  private timer?: ReturnType<typeof setTimeout>;

  override disconnectedCallback(): void {
    void this.stopLearn();
    super.disconnectedCallback();
  }
  private emit(v: KeyMap) {
    this.dispatchEvent(new CustomEvent<KeyMap>('change', { detail: v }));
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
  private setRow(r: Pick<KeyRow, 'id' | 'was'>, v: string) {
    this.emit(setRowKey(this.value, r, v === NONE ? null : v));
  }
  private assignPressed(p: Pressed, v: string) {
    const c = this.codes;
    if (!c || !v) return void (this.err = 'Choose which key it presses.');
    this.err = '';
    const id = nameOfUsage(c, p.page, p.usage);
    const row = keyRows(c, this.value).find((r) => r.page === p.page && r.usage === p.usage);
    const key = v === NONE ? null : v;
    this.emit(row ? setRowKey(this.value, { id: row.id, ...(row.was !== undefined ? { was: row.was } : {}) }, key) : key === null ? this.value : { ...this.value, [id]: key });
    this.assign = '';
  }
  private rowLabel(p: Pressed): string {
    const n = nameOfUsage(this.codes, p.page, p.usage);
    return n.startsWith('HID:') ? `${pageName(p.page)} ${n.slice(4)}` : n;
  }
  /** A grouped key picker. `current` null = "Does nothing"; `blank` adds a "Choose a key…" first option (learn mode). */
  private picker(label: string, current: string | null, on: (v: string) => void, blank = false) {
    const c = this.codes;
    if (!c) return nothing;
    const val = current === null ? NONE : current;
    return html`<select aria-label=${label} @change=${(e: Event) => on((e.target as HTMLSelectElement).value)}>
      ${blank ? html`<option value="" ?selected=${current === ''}>Choose a key…</option>` : nothing}
      <option value=${NONE} ?selected=${val === NONE}>Does nothing</option>
      ${keyOptions(c, current || null).map((g) => html`<optgroup label=${g.group}>${g.keys.map((k) => html`<option value=${k.id} title=${k.id} ?selected=${val === k.id}>${k.label}</option>`)}</optgroup>`)}
    </select>`;
  }

  protected override render() {
    const c = this.codes;
    if (!c) return html`<p class="hint" role="status">Loading the key list…</p>`;
    const rows = keyRows(c, this.value);
    const p = this.pressed;
    const cur = p ? rows.find((r) => r.page === p.page && r.usage === p.usage) : undefined;
    const unmapped = this.seen.filter((s) => !isMapped(rows, s.page, s.usage));
    return html`
      <section class="card learn" aria-label="Learn a button">
        <h3>Press a button on the remote</h3>
        <p>${this.learning ? 'Listening. Press any button on the remote; it appears here.' : 'Not sure which usage a button sends? Start listening, press it, then choose its key.'}</p>
        <div class="row">
          ${this.learning ? html`<button class="btn" @click=${() => void this.stopLearn()}>Stop listening</button><span class="pulse" aria-hidden="true"></span>` : html`<button class="btn pri" @click=${() => void this.startLearn()}>Learn a button</button>`}
        </div>
        <div role="status" aria-live="polite">
          ${p
            ? html`<div class="got"><span>You pressed</span><b>${this.rowLabel(p)}</b><span class="muted mono">${hidCode(p.page, p.usage).slice(4)}</span>
                ${cur?.key ? html`<span>→ acts as <b>${keyText(cur.key)}</b></span>` : html`<span class="chip ir">${cur ? 'Does nothing' : 'Not mapped'}</span>`}
                ${this.picker(`Key for ${this.rowLabel(p)}`, this.assign, (v) => (this.assign = v), true)}
                <button class="btn sm pri" @click=${() => this.assignPressed(p, this.assign)}>${cur?.key ? 'Change' : 'Assign'}</button></div>`
            : nothing}
        </div>
        ${this.err ? html`<span class="err" role="alert">${this.err}</span>` : nothing}
        ${unmapped.length ? html`<div class="unm"><span class="muted">Pressed but not mapped:</span>${unmapped.map((u) => html`<button class="btn sm" @click=${() => { this.pressed = u; this.assign = ''; }}>${this.rowLabel(u)}</button>`)}</div>` : nothing}
      </section>
      <section class="tbl" aria-label="Button map">
        <h3>What each button does</h3>
        <p class="hint">Defaults are filled in. Change a key to override it, or choose "Does nothing" to turn a button off.</p>
        <div role="list" aria-label="Remote buttons and their keys">
          <div class="head" aria-hidden="true"><span>Button</span><span>Usage</span><span>Acts as key</span><span></span></div>
          ${rows.map(
            (r) => html`<div class="kr ${p && r.page === p.page && r.usage === p.usage ? 'hit' : ''}" role="listitem">
              <b title=${r.id}>${r.id.startsWith('HID:') ? 'Your own code' : r.id}</b>
              <span class="mono muted use">${r.page ? hidCode(r.page, r.usage).slice(4) : r.id}</span>
              <span class="pick">${this.picker(`Key for ${r.id}`, r.key, (v) => this.setRow(r, v))}</span>
              <span class="end">${r.was === undefined ? html`<button class="btn sm" aria-label="Remove ${r.id}" @click=${() => this.emit(dropRowKey(this.value, r.id))}>Remove</button>` : r.mine ? html`<button class="btn sm" aria-label="Reset ${r.id} to ${keyText(r.was)}" @click=${() => this.emit(dropRowKey(this.value, r.id))}>Reset</button>` : html`<span class="chip">Default</span>`}</span></div>`,
          )}
        </div>
      </section>`;
  }
}
