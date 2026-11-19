import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { BtCodes } from '../lib/api';
import { hidCode, pageName, parseHid, resolveId, searchBuiltins } from '../lib/bt-model';
import { byteCounter, trimBytes } from '../lib/codes';
import { shared } from '../lib/styles';

/** Pick a Bluetooth device's commands: tick built-in names (searchable) and add your own `HID:` codes. `value` maps a command
 *  name to a built-in name or `HID:pp:uuuu`; every change fires `change` with the whole new map. */
@customElement('hh-bt-commands')
export class HhBtCommands extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: grid; gap: 12px; min-width: 0; }
      .bar { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
      input[type="search"], input[type="text"] { height: 36px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; min-width: 0; }
      .bar input[type="search"] { flex: 1; min-width: 160px; }
      .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 6px; max-height: 340px; overflow: auto; padding: 2px; }
      .chk { display: flex; gap: 8px; align-items: center; min-height: 40px; padding: 4px 10px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); cursor: pointer; min-width: 0; }
      .chk:has(input:checked) { border-color: var(--acc); background: var(--accs); }
      .chk span { display: grid; min-width: 0; }
      .chk b { font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .chk small { color: var(--tx3); font-family: var(--mono); font-size: 11px; }
      h3 { margin: 4px 0 0; font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); }
      .own { display: grid; gap: 6px; }
      .orow { display: flex; gap: 8px; align-items: center; padding: 4px 10px; min-height: 40px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); }
      .orow b { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .add { display: flex; gap: 8px; flex-wrap: wrap; align-items: flex-end; }
      .add label { display: grid; gap: 3px; font-size: 12px; font-weight: 600; color: var(--tx2); }
      .add input { width: 200px; max-width: 100%; }
      .err { color: var(--bad); font-size: 12.5px; }
      .hint { color: var(--tx3); font-size: 12.5px; margin: 0; }
    `,
  ];
  @property({ attribute: false }) codes: BtCodes | null = null;
  @property({ attribute: false }) value: Record<string, string> = {};
  @state() private q = '';
  @state() private cname = '';
  @state() private ccode = '';
  @state() private cerr = '';

  private emit(v: Record<string, string>) {
    this.dispatchEvent(new CustomEvent<Record<string, string>>('change', { detail: v }));
  }
  private toggle(name: string, on: boolean) {
    const v = { ...this.value };
    if (on) v[name] = name;
    else delete v[name];
    this.emit(v);
  }
  private defaults(): Record<string, string> {
    return Object.fromEntries((this.codes?.builtins ?? []).filter((b) => b.default).map((b) => [b.name, b.name]));
  }
  private add() {
    const name = trimBytes(this.cname.trim());
    const code = this.ccode.trim();
    if (!name) return void (this.cerr = 'Give the command a name.');
    if (byteCounter(name).over > 0) return void (this.cerr = 'That name is too long.');
    if (name in this.value) return void (this.cerr = 'Another command already has that name.');
    const ok = this.codes ? resolveId(this.codes, code) : null;
    if (!ok) {
      const h = parseHid(code);
      return void (this.cerr = typeof h === 'string' ? `${h} Or use a built-in name such as VolumeUp.` : 'Unknown code.');
    }
    this.cerr = '';
    this.emit({ ...this.value, [name]: code.toUpperCase().startsWith('HID:') ? hidCode(ok.page, ok.usage) : code });
    this.cname = this.ccode = '';
  }
  protected override render() {
    const c = this.codes;
    if (!c) return html`<p class="hint" role="status">Loading the command list…</p>`;
    const names = new Set(c.builtins.map((b) => b.name));
    const own = Object.entries(this.value).filter(([n, v]) => !(names.has(n) && v === n));
    const list = searchBuiltins(c.builtins, this.q);
    const count = Object.keys(this.value).length;
    return html`
      <div class="bar">
        <input type="search" aria-label="Search commands" placeholder="Search commands" .value=${this.q} @input=${(e: Event) => (this.q = (e.target as HTMLInputElement).value)} />
        <button class="btn sm" @click=${() => this.emit(this.defaults())}>Default set</button>
        <button class="btn sm" ?disabled=${!count} @click=${() => this.emit({})}>Clear</button>
        <span class="muted" role="status">${count} command${count === 1 ? '' : 's'} chosen</span>
      </div>
      <div class="grid" role="group" aria-label="Built-in commands">
        ${list.map((b) => html`<label class="chk"><input type="checkbox" .checked=${this.value[b.name] === b.name} @change=${(e: Event) => this.toggle(b.name, (e.target as HTMLInputElement).checked)} /><span><b>${b.name}</b><small>${pageName(b.page)} ${hidCode(b.page, b.usage).slice(4)}</small></span></label>`)}
        ${list.length ? nothing : html`<p class="hint">No built-in command matches "${this.q}".</p>`}
      </div>
      <h3>Your own codes</h3>
      <div class="own">
        ${own.map(([n, v]) => html`<div class="orow"><b>${n}</b><span class="mono muted">${v}</span><button class="btn sm" aria-label="Remove ${n}" @click=${() => { const x = { ...this.value }; delete x[n]; this.emit(x); }}>Remove</button></div>`)}
        <div class="add">
          <label>Command name<input type="text" .value=${this.cname} placeholder="Netflix" @input=${(e: Event) => (this.cname = (e.target as HTMLInputElement).value)} @keydown=${(e: KeyboardEvent) => e.key === 'Enter' && this.add()} /></label>
          <label>HID code<input type="text" .value=${this.ccode} placeholder="HID:0C:0223" @input=${(e: Event) => (this.ccode = (e.target as HTMLInputElement).value)} @keydown=${(e: KeyboardEvent) => e.key === 'Enter' && this.add()} /></label>
          <button class="btn sm" @click=${() => this.add()}>Add code</button>
        </div>
        ${this.cerr ? html`<span class="err" role="alert">${this.cerr}</span>` : html`<p class="hint">For a key that is not in the list. Page 07 is the keyboard, 0C the consumer (media) page; both numbers are hex.</p>`}
      </div>`;
  }
}
