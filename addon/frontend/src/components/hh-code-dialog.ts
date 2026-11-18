import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { ApiError, type IrParsed } from '../lib/api';
import { PROTOCOLS, byteCounter, kindOf, summarize, validateCode, type CodeKind, trimBytes } from '../lib/codes';
import { nameProblem, withPorts, withRepeats } from '../lib/device-edit';
import type { Device, IrAction } from '../lib/model';
import { describeRef, findRefs } from '../lib/refs';
import { getStore } from '../lib/store';
import { shared } from '../lib/styles';
import './hh-dialog';
import './hh-send-ir';

export interface CodeEdit {
  /** which list the code lives in */
  where: 'commands' | 'inputs' | 'power';
  /** the name before editing (null: a new command or input); for power: on, off or toggle */
  name: string | null;
  code: IrAction;
}
export interface CodeSave extends CodeEdit {
  newName: string;
}
const TABS: [string, string][] = [['proto', 'Protocol'], ['pronto', 'Pronto'], ['raw', 'Raw'], ['import', 'Paste or upload']];

/** Code editor dialog: name with byte counter, protocol / Pronto / raw / pasted file, IR ports, repeats, Send IR. */
@customElement('hh-code-dialog')
export class HhCodeDialog extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: contents; }
      .f { display: grid; gap: 4px; font-size: 12px; font-weight: 600; color: var(--tx2); }
      input[type="text"], input[type="number"], select, textarea { border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; height: 36px; width: 100%; min-width: 0; font: inherit; font-weight: 400; }
      textarea { height: 96px; padding: 8px 10px; font-family: var(--mono); font-size: 12px; resize: vertical; }
      .cnt { display: flex; justify-content: space-between; font-weight: 500; color: var(--tx3); }
      .cnt.over { color: var(--bad); }
      .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: end; }
      .row > * { flex: 1 1 110px; }
      .tabs { display: flex; gap: 2px; background: var(--sf2); padding: 2px; border-radius: var(--r1); flex-wrap: wrap; }
      .tabs button { flex: 1; height: 30px; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); font-weight: 600; font-size: 12px; cursor: pointer; padding: 0 8px; }
      .tabs button[aria-selected="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.14); }
      .err { color: var(--bad); font-size: 12px; font-weight: 500; }
      .hint { color: var(--tx3); font-size: 12px; font-weight: 400; }
      .ports { display: flex; gap: 6px; }
      .ports button { width: 44px; height: 36px; border-radius: var(--r1); border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 700; cursor: pointer; }
      .ports button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .pick { display: grid; gap: 4px; max-height: 150px; overflow: auto; }
      .pick label { display: flex; gap: 8px; align-items: center; font-weight: 400; color: var(--tx); padding: 4px 6px; border-radius: var(--r1); }
      .pick label:hover { background: var(--sf2); }
      ul { margin: 0; padding-left: 18px; font-size: 12px; color: var(--tx2); }
      .fl { margin-right: auto; display: flex; gap: 8px; }
      .send { display: flex; align-items: center; gap: 10px; padding: 8px 10px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf2); }
    `,
  ];
  @property({ attribute: false }) device: Device | null = null;
  @property({ attribute: false }) edit: CodeEdit | null = null;
  @property({ type: Boolean }) ha25 = false;
  @state() private tab = 'proto';
  @state() private name = '';
  @state() private code: IrAction = {};
  @state() private pasteKind: 'pronto' | 'flipper' | 'smartir' = 'pronto';
  @state() private text = '';
  @state() private parsed: IrParsed | null = null;
  @state() private parseErr = '';
  @state() private busy = false;
  private seen: CodeEdit | null = null;

  protected override willUpdate(): void {
    if (this.edit !== this.seen) {
      this.seen = this.edit;
      if (!this.edit) return;
      this.name = this.edit.name && this.edit.where !== 'power' ? this.edit.name : '';
      this.code = JSON.parse(JSON.stringify(this.edit.code)) as IrAction;
      this.tab = kindOf(this.code) === 'pronto' ? 'pronto' : kindOf(this.code) === 'raw' ? 'raw' : 'proto';
      this.parsed = null;
      this.parseErr = '';
      this.text = '';
    }
  }

  private setKind(k: CodeKind) {
    const keep = { ...(this.code.port_mask ? { port_mask: this.code.port_mask } : {}), ...(this.code.repeats ? { repeats: this.code.repeats } : {}) };
    if (k === 'proto') this.code = { proto: 'nec', addr: 0, cmd: 0, ...keep };
    else if (k === 'pronto') this.code = { pronto: '', ...keep };
    else this.code = { raw: { hz: 38000, us: [] }, ...keep };
  }
  private pickTab(t: string) {
    this.tab = t;
    if (t === 'proto' && kindOf(this.code) !== 'proto') this.setKind('proto');
    if (t === 'pronto' && kindOf(this.code) !== 'pronto') this.setKind('pronto');
    if (t === 'raw' && kindOf(this.code) !== 'raw') this.setKind('raw');
  }
  private problem(): string | null {
    if (!this.edit || !this.device) return 'Nothing to edit';
    if (this.edit.where !== 'power') {
      const p = nameProblem(this.device, this.name, this.edit.name ?? undefined, this.edit.where === 'inputs' ? 'input' : 'command');
      if (p) return p;
    }
    const v = validateCode(this.code);
    return v.ok ? null : (Object.values(v.errors)[0] ?? 'The code is not valid');
  }
  private save() {
    if (this.problem() || !this.edit) return;
    this.dispatchEvent(new CustomEvent<CodeSave>('hh-code-save', { detail: { ...this.edit, code: this.code, newName: this.name.trim() }, bubbles: true, composed: true }));
  }
  private async parse() {
    this.busy = true;
    this.parseErr = '';
    this.parsed = null;
    try {
      this.parsed = await getStore().api.irParse(this.pasteKind, this.text);
    } catch (e) {
      this.parseErr = e instanceof ApiError ? (e.errors[0]?.msg ?? e.message) : 'Could not parse that.';
    }
    this.busy = false;
  }
  private use(a: IrAction, name: string) {
    this.code = { ...a, ...(this.code.port_mask ? { port_mask: this.code.port_mask } : {}) };
    this.tab = kindOf(a) === 'pronto' ? 'pronto' : kindOf(a) === 'raw' ? 'raw' : 'proto';
    if (!this.name && this.edit?.where !== 'power') this.name = trimBytes(name);
  }
  private num(k: 'addr' | 'cmd', v: string) {
    this.code = { ...this.code, [k]: v === '' ? undefined : Number(v) };
  }

  private fields() {
    const errs = validateCode(this.code).errors;
    const k = kindOf(this.code);
    if (this.tab === 'proto' && k === 'proto') {
      const lim = this.code.proto ? PROTOCOLS[this.code.proto] : undefined;
      return html`<div class="row">
          <label class="f">Protocol<select aria-label="Protocol" @change=${(e: Event) => (this.code = { ...this.code, proto: (e.target as HTMLSelectElement).value })}>${Object.keys(PROTOCOLS).map((p) => html`<option value=${p} ?selected=${p === this.code.proto}>${p}</option>`)}</select></label>
          <label class="f">Address<input type="number" aria-label="Address" min="0" .value=${String(this.code.addr ?? '')} @input=${(e: Event) => this.num('addr', (e.target as HTMLInputElement).value)} />${lim ? html`<span class="hint">0 to ${lim.addr}</span>` : nothing}</label>
          <label class="f">Command<input type="number" aria-label="Command number" min="0" .value=${String(this.code.cmd ?? '')} @input=${(e: Event) => this.num('cmd', (e.target as HTMLInputElement).value)} />${lim ? html`<span class="hint">0 to ${lim.cmd}</span>` : nothing}</label>
        </div>
        ${['proto', 'addr', 'cmd'].map((f) => (errs[f] ? html`<span class="err" role="alert">${errs[f]}</span>` : nothing))}`;
    }
    if (this.tab === 'pronto' && k === 'pronto')
      return html`<label class="f">Pronto code<textarea aria-label="Pronto code" .value=${this.code.pronto ?? ''} @input=${(e: Event) => (this.code = { ...this.code, pronto: (e.target as HTMLTextAreaElement).value })}></textarea></label>${errs['pronto'] ? html`<span class="err" role="alert">${errs['pronto']}</span>` : nothing}`;
    if (this.tab === 'raw' && k === 'raw') {
      const r = this.code.raw as { hz: number; us: number[] };
      return html`<div class="row"><label class="f">Carrier (Hz)<input type="number" aria-label="Carrier Hz" .value=${String(r.hz)} @input=${(e: Event) => (this.code = { ...this.code, raw: { ...r, hz: Number((e.target as HTMLInputElement).value) } })} /></label></div>
        <label class="f">Mark and space durations (µs)<textarea aria-label="Durations" .value=${r.us.join(' ')} @input=${(e: Event) => (this.code = { ...this.code, raw: { ...r, us: (e.target as HTMLTextAreaElement).value.split(/[\s,]+/).filter(Boolean).map(Number) } })}></textarea></label>
        ${['hz', 'us'].map((f) => (errs[f] ? html`<span class="err" role="alert">${errs[f]}</span>` : nothing))}`;
    }
    if (this.tab === 'import') {
      return html`<label class="f">Format<select aria-label="Format" @change=${(e: Event) => (this.pasteKind = (e.target as HTMLSelectElement).value as typeof this.pasteKind)}>
          <option value="pronto" ?selected=${this.pasteKind === 'pronto'}>Pronto hex</option><option value="flipper" ?selected=${this.pasteKind === 'flipper'}>Flipper .ir file</option><option value="smartir" ?selected=${this.pasteKind === 'smartir'}>SmartIR .json</option></select></label>
        <label class="f">Paste the text, or choose a file<textarea aria-label="Pasted codes" .value=${this.text} @input=${(e: Event) => (this.text = (e.target as HTMLTextAreaElement).value)}></textarea></label>
        <div class="row"><input type="file" aria-label="Upload a file" accept=".ir,.json,.txt" @change=${async (e: Event) => { const f = (e.target as HTMLInputElement).files?.[0]; if (f) { this.text = await f.text(); if (f.name.endsWith('.ir')) this.pasteKind = 'flipper'; else if (f.name.endsWith('.json')) this.pasteKind = 'smartir'; } }} />
          <button class="btn sm" ?disabled=${this.busy || !this.text.trim()} @click=${() => void this.parse()}>${this.busy ? 'Reading…' : 'Read codes'}</button></div>
        ${this.parseErr ? html`<span class="err" role="alert">${this.parseErr}</span>` : nothing}
        ${this.parsed ? html`<div class="pick" role="radiogroup" aria-label="Codes found">${this.parsed.actions.map((a) => html`<label><input type="radio" name="found" @change=${() => this.use(a.action, a.name)} /> <b>${a.name}</b> <span class="muted">${summarize(a.action)}</span></label>`)}</div>
          ${this.parsed.skipped.length ? html`<span class="hint">Skipped ${this.parsed.skipped.length}: ${this.parsed.skipped.slice(0, 3).map((s) => `${s.name} (${s.reason})`).join(', ')}</span>` : nothing}` : nothing}`;
    }
    return nothing;
  }

  protected override render() {
    const e = this.edit;
    const d = this.device;
    if (!e || !d) return html`<hh-dialog .open=${false}></hh-dialog>`;
    const store = getStore();
    const bytes = byteCounter(this.name);
    const mask = this.code.port_mask ?? 7;
    const uses = e.name && e.where !== 'power' ? findRefs(store.config, e.where === 'commands' ? { device: d.id, command: e.name } : { device: d.id, input: e.name }) : [];
    const prob = this.problem();
    const title = e.where === 'power' ? `Power ${e.name}` : e.name ? `Edit ${e.name}` : e.where === 'inputs' ? 'New input' : 'New command';
    return html`<hh-dialog .open=${true} heading=${title} @close=${() => this.dispatchEvent(new Event('hh-code-close', { bubbles: true, composed: true }))}>
      ${e.where !== 'power'
        ? html`<label class="f">Name<span class="cnt ${bytes.over ? 'over' : ''}"><span>${bytes.hint || 'Shown in the palette'}</span><span>${bytes.used}/${bytes.max} B</span></span>
            <input type="text" aria-label="Name" .value=${this.name} @input=${(ev: Event) => (this.name = (ev.target as HTMLInputElement).value)} /></label>
          ${this.name && nameProblem(d, this.name, e.name ?? undefined, e.where === 'inputs' ? 'input' : 'command') && !bytes.over ? html`<span class="err" role="alert">${nameProblem(d, this.name, e.name ?? undefined, e.where === 'inputs' ? 'input' : 'command')}</span>` : nothing}`
        : nothing}
      <div class="tabs" role="tablist" aria-label="Kind of code">${TABS.map(([id, label]) => html`<button role="tab" aria-selected=${this.tab === id} @click=${() => this.pickTab(id)}>${label}</button>`)}</div>
      ${this.fields()}
      <div class="row">
        <div class="f">IR ports<div class="ports" role="group" aria-label="IR ports">${[1, 2, 4].map((bit, i) => html`<button aria-pressed=${(mask & bit) !== 0} aria-label="Port ${i + 1}" @click=${() => { const m = mask ^ bit; this.code = withPorts(this.code, m === 0 ? 7 : m); }}>${i + 1}</button>`)}</div><span class="hint">${mask === 7 ? 'All ports' : `Only ports ${[1, 2, 4].map((b, i) => (mask & b ? i + 1 : 0)).filter(Boolean).join(' and ')}`}</span></div>
        ${this.ha25 ? html`<label class="f">Repeats<input type="number" min="1" max="8" aria-label="Repeats" .value=${String(this.code.repeats ?? 1)} @input=${(ev: Event) => (this.code = withRepeats(this.code, Number((ev.target as HTMLInputElement).value)))} /><span class="hint">1 to 8 frames in total</span></label>` : nothing}
      </div>
      <div class="send"><hh-send-ir .code=${this.code} name=${this.name || e.name || 'this code'}></hh-send-ir><span class="hint">Sends this draft code once through Home Assistant. Nothing is sent until you press it.</span></div>
      ${uses.length ? html`<div class="f">Used by<ul>${uses.slice(0, 8).map((r) => html`<li>${describeRef(r)}</li>`)}${uses.length > 8 ? html`<li>and ${uses.length - 8} more</li>` : nothing}</ul></div>` : nothing}
      ${e.name && e.where !== 'power' ? html`<div slot="footer" class="fl">
        ${e.where === 'commands' ? html`<button class="btn" @click=${() => this.dispatchEvent(new CustomEvent('hh-code-dup', { detail: e.name, bubbles: true, composed: true }))}>Duplicate</button>` : nothing}
        <button class="btn danger" @click=${() => this.dispatchEvent(new CustomEvent('hh-code-delete', { detail: e, bubbles: true, composed: true }))}>Delete</button></div>` : nothing}
      <button slot="footer" class="btn" @click=${() => this.dispatchEvent(new Event('hh-code-close', { bubbles: true, composed: true }))}>Cancel</button>
      <button slot="footer" class="btn pri" ?disabled=${!!prob} title=${prob ?? ''} @click=${() => this.save()}>Save</button>
    </hh-dialog>`;
  }
}
