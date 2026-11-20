import { displayKey } from '../lib/key-names';
import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { setStyle } from '../lib/set-style';
import { shared } from '../lib/styles';
import { PALETTE_NAMES, css as hex6 } from '../lib/themes';
import { needsOutline } from '../lib/theme-math';
import { contrastInfo, loadRecent, nearestSlot, pushRecent, ratioText, saveRecent } from '../lib/theme-edit';
import { hexOf, isFree, parseHex, resolveColour, type RenderTheme } from '../lib/theme-model';

export interface ColourDetail {
  /** palette name, `#RRGGBB`, or undefined for the role's own colour */
  value: string | undefined;
}
export interface AddPaletteDetail {
  hex: string;
  slot: number;
}

/** Colour row of the widget inspector: Default + the 12 swatches of the active theme + Custom... (hex, native picker, recents, contrast, add to theme). */
@customElement('hh-colour-field')
export class HhColourField extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; }
      h4 { margin: 0 0 6px; font-size: 12px; font-weight: 650; color: var(--tx2); }
      .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(52px, 1fr)); gap: 6px; }
      .sw { display: grid; gap: 3px; justify-items: stretch; padding: 4px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx2); font-size: 11px; cursor: pointer; text-transform: capitalize; min-width: 0; }
      .sw[aria-pressed="true"] { border-color: var(--acc); box-shadow: 0 0 0 1px var(--acc); color: var(--tx); }
      .sw i { display: grid; place-items: center; height: 26px; border-radius: 7px; font-style: normal; font-weight: 650; font-size: 13px; border: 1px solid transparent; }
      .sw i.ol { border-color: #687888; }
      .sw span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; text-align: center; }
      .sw.def i { background: var(--sf2); color: var(--tx2); border: 1px dashed var(--line); font-size: 11px; }
      .sw.cus i { border: 2px dashed var(--tx2); font-size: 10px; font-family: var(--mono); }
      .pop { margin-top: 8px; padding: 10px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf2); display: grid; gap: 10px; }
      .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
      input[type="text"], select { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; }
      input[type="text"] { width: 10.5ch; font-family: var(--mono); }
      input[type="color"] { width: 44px; height: 34px; padding: 2px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); cursor: pointer; }
      .rec { display: flex; gap: 4px; flex-wrap: wrap; align-items: center; min-height: 28px; }
      .rec button { width: 28px; height: 28px; border-radius: 14px; border: 2px solid var(--line); cursor: pointer; padding: 0; }
      .rec button[aria-pressed="true"] { border-color: var(--tx); }
      .lbl { font-size: 11px; font-weight: 650; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); }
      .read { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; font-size: 12px; }
      .why { margin: 0; font-size: 12px; color: var(--tx3); }
    `,
  ];
  @property({ attribute: false }) theme!: RenderTheme;
  @property() value: string | undefined;
  @property() heading = 'Colour';
  @property() defaultLabel = 'Default (role)';
  /** the look the widget has with no colour set: [fill, ink] */
  @property({ attribute: false }) defaultLook: [number, number] = [0x202c38, 0xf8fcf8];
  @property({ type: Boolean }) canAdd = true;
  @state() private open = false;
  @state() private draft = '';
  @state() private recent: string[] = loadRecent();
  @state() private repl = 0;

  protected override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has('value') && isFree(this.value)) {
      this.draft = this.value.toUpperCase();
      this.open = true;
    }
  }
  private emit(value: string | undefined, coalesce = false) {
    this.dispatchEvent(new CustomEvent<ColourDetail & { coalesce: boolean }>('colour', { detail: { value, coalesce }, bubbles: true, composed: true }));
  }
  private remember(h: string) {
    this.recent = pushRecent(this.recent, h);
    saveRecent(this.recent);
  }
  private onHex(e: Event) {
    const t = (e.target as HTMLInputElement).value;
    this.draft = t;
    const h = parseHex(t);
    if (h) {
      this.repl = nearestSlot(h, this.theme);
      this.emit(h, true);
    }
  }
  private commit() {
    const h = parseHex(this.draft);
    if (h) {
      this.draft = h;
      this.remember(h);
    } else this.draft = isFree(this.value) ? this.value : '';
  }
  private onNative(e: Event) {
    const h = (e.target as HTMLInputElement).value.toUpperCase();
    this.draft = h;
    this.repl = nearestSlot(h, this.theme);
    this.emit(h, true);
    if (e.type === 'change') this.remember(h);
  }

  private swatches() {
    const th = this.theme;
    const free = isFree(this.value);
    const [df, di] = this.defaultLook;
    return html`<div class="grid" role="group" aria-label=${this.heading}>
      <button class="sw def" aria-pressed=${!this.value} title=${this.defaultLabel} aria-label=${this.defaultLabel} @click=${() => { this.open = false; this.emit(undefined); }}>
        <i style=${setStyle({ background: hex6(df), color: hex6(di), border: '1px solid var(--line)' })}>Aa</i><span>Default</span></button>
      ${th.swatches.map((s) => html`<button class="sw" aria-pressed=${this.value === s.name} title=${displayKey(s.name)} aria-label=${displayKey(s.name)} @click=${() => { this.open = false; this.emit(s.name); }}>
        <i class=${needsOutline(s.fill, th.tokens.bg) ? 'ol' : ''} style=${setStyle({ background: hex6(s.fill), color: hex6(s.ink) })}>Aa</i><span>${displayKey(s.name)}</span></button>`)}
      <button class="sw cus" aria-pressed=${free} aria-expanded=${this.open} title="Any colour" @click=${() => (this.open = !this.open)}>
        <i style=${setStyle(free ? { background: hex6(contrastBg(this.value as string, th)), color: hex6(contrastInk(this.value as string, th)) } : { background: 'var(--sf)', color: 'var(--tx2)' })}>${free ? (this.value as string).toUpperCase() : 'Aa'}</i><span>Custom…</span></button>
    </div>`;
  }

  private customPanel() {
    const th = this.theme;
    const h = parseHex(this.draft);
    const cur = isFree(this.value) ? this.value : (h ?? undefined);
    const info = cur ? contrastInfo(cur, th) : null;
    return html`<div class="pop" role="group" aria-label="Custom colour">
      <div class="row">
        <label class="lbl" for="hx">Hex</label>
        <input id="hx" type="text" inputmode="text" maxlength="7" spellcheck="false" autocomplete="off" aria-label="Hex colour" placeholder="#RRGGBB" .value=${this.draft}
          aria-invalid=${this.draft && !h ? 'true' : 'false'} @input=${(e: Event) => this.onHex(e)} @change=${() => this.commit()} />
        <input type="color" aria-label="Pick a colour" .value=${(h ?? (isFree(this.value) ? this.value : hexOf(th.tokens.acc))).toLowerCase()} @input=${(e: Event) => this.onNative(e)} @change=${(e: Event) => this.onNative(e)} />
      </div>
      <div><div class="lbl">Recent</div><div class="rec" role="group" aria-label="Recent colours">
        ${this.recent.length
          ? this.recent.map((r) => html`<button style=${setStyle({ background: r })} aria-label=${r} title=${r} aria-pressed=${cur === r} @click=${() => { this.draft = r; this.repl = nearestSlot(r, th); this.emit(r); this.remember(r); }}></button>`)
          : html`<span class="why">Colours you use appear here.</span>`}
      </div></div>
      ${info
        ? html`<div class="read" role="status">
            <span class="chip ${info.ok ? 'ok' : 'ir'}">${info.ok ? '' : 'Low contrast: '}label contrast ${ratioText(info.ratio_x100)}:1</span>
            ${!info.ok && info.fix ? html`<button class="btn sm" @click=${() => { const f = info.fix as string; this.draft = f; this.emit(f); this.remember(f); }}>Use nearest readable (${info.fix})</button>` : nothing}
          </div>`
        : html`<p class="why">Type a hex colour such as #3355FF. The remote shows the nearest of its 65,536 colours.</p>`}
      ${this.canAdd && cur
        ? html`<div class="row">
            <label class="lbl" for="slot">Replace</label>
            <select id="slot" aria-label="Palette colour to replace" .value=${String(this.repl)} @change=${(e: Event) => (this.repl = Number((e.target as HTMLSelectElement).value))}>
              ${PALETTE_NAMES.map((n, i) => html`<option value=${i} ?selected=${i === this.repl}>${n}</option>`)}
            </select>
            <button class="btn sm" @click=${() => this.dispatchEvent(new CustomEvent<AddPaletteDetail>('add-palette', { detail: { hex: cur, slot: this.repl }, bubbles: true, composed: true }))}>Add to My Theme's Palette</button>
          </div>`
        : nothing}
    </div>`;
  }

  protected override render() {
    if (!this.theme) return nothing;
    return html`<h4>${this.heading}</h4>${this.swatches()}${this.open ? this.customPanel() : nothing}`;
  }
}

function contrastBg(spec: string, th: RenderTheme): number {
  return resolveColour(spec, th)?.fill ?? th.tokens.s1;
}
function contrastInk(spec: string, th: RenderTheme): number {
  return resolveColour(spec, th)?.ink ?? th.tokens.tx;
}
