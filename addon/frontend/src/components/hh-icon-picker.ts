import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { displayKey } from '../lib/key-names';
import { isNewIcon, suggestIcons } from '../lib/icon-suggest';
import { stockIconSvg } from '../lib/icon-shapes';
import { STOCK_ICONS, type StockIcon } from '../lib/stock-icons';
import { shared } from '../lib/styles';

/** Picker sections, in order; an icon appears in the first section that lists it. Proposed glyphs (not yet in the firmware) get their own. */
const SECTIONS: { title: string; names: string[] }[] = [
  { title: 'Media', names: ['play', 'pause', 'stop', 'record', 'rewind', 'ffwd', 'skip_next', 'skip_prev', 'replay10', 'skip10', 'shuffle', 'repeat', 'vol_up', 'vol_down', 'mute', 'audio', 'audio_track', 'cc', 'music', 'film', 'picture', 'aspect'] },
  { title: 'Navigation', names: ['up', 'down', 'chev_left', 'chev_right', 'back', 'home', 'menu', 'check', 'enter', 'search', 'grid_apps', 'list', 'guide', 'info', 'settings', 'input', 'ch', 'prev_ch', 'numpad', 'keyboard', 'sleep_timer'] },
  { title: 'Home & Devices', names: ['tv', 'game', 'radio', 'app', 'power', 'bulb', 'scene', 'thermo', 'blinds', 'switch', 'fan', 'lock', 'sun', 'moon', 'star', 'plus', 'minus', 'sync', 'warn', 'rf', 'rf_off'] },
];

/**
 * A searchable picker of the remote's stock icons. `value` is the icon name ('' = the default); fires `hh-icon` with the new name or undefined.
 * With `command` / `label` it opens on a Suggested row (best matches first, the reason it matched is each button's tooltip).
 */
@customElement('hh-icon-picker')
export class HhIconPicker extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: block; }
      input[type="search"] { height: 34px; width: 100%; box-sizing: border-box; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; margin-bottom: 8px; }
      .scroll { max-height: 260px; overflow: auto; padding: 2px; }
      h4 { margin: 10px 0 4px; font-size: 11px; letter-spacing: .06em; text-transform: uppercase; color: var(--tx2); }
      h4:first-child { margin-top: 0; }
      .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(44px, 1fr)); gap: 6px; }
      button.pick { position: relative; height: 44px; display: grid; place-items: center; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf2); color: var(--tx); cursor: pointer; font-size: 11.5px; }
      button.pick:hover { background: var(--sf3); }
      button.pick[aria-pressed="true"] { background: var(--acc); color: var(--onacc); border-color: var(--acc); }
      button.pick .sic { width: 24px; height: 24px; }
      .sug button.pick { height: 56px; grid-template-rows: 24px auto; grid-template-columns: minmax(0, 1fr); min-width: 0; overflow: hidden; gap: 2px; padding: 4px 2px 2px; }
      .sug button.pick .nm { font-size: 9.5px; line-height: 1.1; min-width: 0; max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .new::after { content: 'new'; position: absolute; top: -5px; right: -4px; font-size: 9px; line-height: 1; padding: 2px 4px; border-radius: 6px; background: var(--acc); color: var(--onacc); }
      .none { margin: 8px 0 0; }
    `,
  ];
  @property() value = '';
  /** the command the widget runs, its label and its device's icon: what the Suggested row matches */
  @property() command = '';
  @property() label = '';
  @property({ attribute: 'device-icon' }) deviceIcon = '';
  @state() private q = '';
  private pick(name: string | undefined): void {
    this.dispatchEvent(new CustomEvent('hh-icon', { detail: name, bubbles: true, composed: true }));
  }
  private btn(i: StockIcon, why = '') {
    const name = displayKey(i.name);
    const tip = why ? `${name}: ${why}` : name;
    return html`<button class="pick ${isNewIcon(i) ? 'new' : ''}" aria-pressed=${this.value === i.name} aria-label=${tip} title=${tip} @click=${() => this.pick(i.name)}>${stockIconSvg(i.id)}${why ? html`<span class="nm">${name}</span>` : nothing}</button>`;
  }
  protected override render() {
    const q = this.q.trim().toLowerCase();
    const found = STOCK_ICONS.filter((i) => !q || i.name.includes(q) || displayKey(i.name).toLowerCase().includes(q) || i.keywords.some((k) => k.includes(q)));
    const sug = q ? [] : suggestIcons(this.command, this.label, this.deviceIcon).slice(0, 6);
    const shown = new Set<string>();
    const groups = [
      ...SECTIONS.map((s) => ({ title: s.title, icons: s.names.flatMap((n) => found.filter((i) => i.name === n && !isNewIcon(i))) })),
      { title: 'Proposed Glyphs', icons: found.filter(isNewIcon) },
    ].map((g) => ({ ...g, icons: g.icons.filter((i) => !shown.has(i.name) && shown.add(i.name)) }));
    const rest = found.filter((i) => !shown.has(i.name));
    if (rest.length) groups.push({ title: 'More', icons: rest });
    return html`<input type="search" placeholder="Search icons" aria-label="Search icons" .value=${this.q} @input=${(e: Event) => (this.q = (e.target as HTMLInputElement).value)} />
      <div class="scroll" role="group" aria-label="Icon">
        ${sug.length ? html`<h4>Suggested</h4><div class="grid sug">${sug.map((s) => this.btn(s.icon, s.reason))}</div>` : nothing}
        ${groups.filter((g) => g.icons.length).map((g) => html`<h4>${g.title}</h4><div class="grid">${g.icons.map((i) => this.btn(i))}</div>`)}
      </div>
      ${found.length ? '' : html`<p class="muted">No icon matches.</p>`}
      <button class="btn sm none" ?disabled=${!this.value} @click=${() => this.pick(undefined)}>Use the Default Icon</button>`;
  }
}
declare global { interface HTMLElementTagNameMap { 'hh-icon-picker': HhIconPicker } }
