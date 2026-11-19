import { css, html, nothing, svg } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { V2Page, V2Widget } from '../lib/api';
import { pageGroup } from '../lib/page-renderer';
import { icon } from '../lib/icons';
import { stockIconByName } from '../lib/stock-icons';
import { shared } from '../lib/styles';
import { setStyle } from '../lib/set-style';
import { PALETTE_NAMES, css as hex6, type TokenName } from '../lib/themes';
import {
  PRESETS, PRESET_KEYS, TOKEN_GROUPS, TOKEN_LABEL, activeName, applyFix, deleteTheme, duplicateTheme, exportTheme, fixAll, hexOf, importTheme,
  overridesSlot, overridesToken, parseHex, renameTheme, resetPaletteFill, resetToken, selectTheme, setPaletteFill, setToken, slugOf, themeNamed, themeProblems, themeUsage, userThemeNames,
  ratioText, type RenderTheme,
} from '../lib/theme-model';
import { StoreElement, confirmAction } from '../components/base';
import '../components/hh-mini-remote';
import { statusChip, themeNotice } from '../components/parts';
import type { TileSample } from '../lib/page-renderer';

const ic = (n: string): number => stockIconByName(n)?.id ?? 0;
const sw = (id: number, kind: V2Widget['kind'], x: number, y: number, w: number, label: string, extra: Partial<V2Widget> = {}): V2Widget => ({
  kind, id, x, y, w, h: 1, label, role: 'default', shape: 'card', icon_only: false, icon: 0, bind: null, ...extra,
});
/** A page that shows every colour rule: roles, palette colours, a free colour, tile states. */
export const SAMPLE_PAGE: V2Page = {
  version: 2, id: 0, cols: 3, rows: 5, gen: 0, total: 1, kind: 'normal', back: false, seq: null, act: 0, title: 'Sample', theme: 0,
  widgets: [
    sw(0, 'label', 0, 0, 3, 'Living room', { color: 'blue' }),
    sw(1, 'button', 0, 1, 1, 'Watch', { role: 'primary', icon: ic('tv') }),
    sw(2, 'button', 1, 1, 1, 'Music', { icon: ic('music') }),
    sw(3, 'button', 2, 1, 1, 'More', { role: 'secondary' }),
    sw(4, 'button', 0, 2, 1, 'Red', { color: 'red', icon: ic('power') }),
    sw(5, 'button', 1, 2, 1, 'Green', { color: 'green', icon: ic('play') }),
    sw(6, 'button', 2, 2, 1, 'Custom', { color: '#E0B040' }),
    sw(7, 'tile', 0, 3, 1, 'Lamp', { icon: ic('bulb') }),
    sw(8, 'tile', 1, 3, 1, 'Temp'),
    sw(9, 'tile', 2, 3, 1, 'Door', { on_color: 'teal' }),
    sw(10, 'button', 0, 4, 3, 'Power Off', { role: 'danger', shape: 'pill' }),
  ],
};
const SAMPLE_TILES: Record<number, TileSample> = { 7: { state: 'on', text: 'On' }, 8: { state: 'value', text: '21.5' }, 9: { state: 'on', text: 'On' } };

@customElement('hh-themes')
export class HhThemes extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      header.top { display: flex; gap: 12px; align-items: flex-start; flex-wrap: wrap; }
      header.top > div { flex: 1 1 280px; display: grid; gap: 4px; }
      h2.t { margin: 0; font-size: 20px; font-weight: 650; }
      .gal { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }
      .tc { display: grid; gap: 10px; align-content: start; }
      .tc.on { border-color: var(--acc); box-shadow: 0 0 0 1px var(--acc); }
      .tc h3 { margin: 0; justify-content: space-between; }
      .thumbs { display: flex; gap: 8px; align-items: flex-start; }
      .thumbs svg { width: calc(50% - 4px); height: auto; border-radius: 8px; box-shadow: 0 0 0 1px var(--line); display: block; }
      .acts { display: flex; gap: 6px; flex-wrap: wrap; }
      .ed { display: grid; grid-template-columns: minmax(0, 1fr) 260px; gap: 16px; align-items: start; }
      @media (max-width: 900px) { .ed { grid-template-columns: minmax(0, 1fr); } }
      .ed .side { position: sticky; top: 64px; display: grid; gap: 10px; justify-items: center; }
      @media (max-width: 900px) { .ed .side { position: static; } }
      .side hh-mini-remote { width: 200px; }
      .side .th { display: flex; gap: 6px; }
      .side .th svg { width: 76px; height: auto; border-radius: 6px; box-shadow: 0 0 0 1px var(--line); display: block; }
      .grp { display: grid; gap: 6px; margin-bottom: 14px; }
      .grp h4, .pal h4 { margin: 0; font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; }
      .tk { display: grid; grid-template-columns: 40px minmax(0, 1fr) auto auto; gap: 8px; align-items: center; padding: 4px 0; border-top: 1px solid var(--line); }
      .tk .nm { display: grid; min-width: 0; }
      .tk .nm b { font-size: 13px; font-weight: 600; }
      .tk .nm span { font-size: 11px; color: var(--tx3); }
      @media (max-width: 520px) { .tk { grid-template-columns: 40px minmax(0, 1fr) auto; } .tk .nm span { display: none; } .tk button.rs { grid-column: 3; } .tk input.hx { grid-column: 3; } }
      input[type="color"] { width: 40px; height: 34px; padding: 2px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); cursor: pointer; }
      input.hx, input.nm2 { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; }
      input.hx { width: 9ch; font-family: var(--mono); }
      input.nm2 { width: 100%; max-width: 280px; }
      input[aria-invalid="true"] { border-color: var(--bad); }
      .pal .row { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 8px; margin-top: 6px; }
      .pc { display: grid; grid-template-columns: 40px minmax(0, 1fr); gap: 4px 8px; align-items: center; padding: 6px; border: 1px solid var(--line); border-radius: var(--r1); }
      .pc .a { grid-column: 1 / -1; display: flex; gap: 6px; align-items: center; justify-content: space-between; }
      .pc .a b { font-size: 12px; text-transform: capitalize; }
      .pc .a code { font-size: 11px; color: var(--tx3); }
      .pc .aa { height: 34px; border-radius: 7px; display: grid; place-items: center; font-weight: 650; font-size: 13px; }
      .prob { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; padding: 6px 0; border-top: 1px solid var(--line); }
      .prob span { flex: 1 1 200px; min-width: 0; }
      .prob i { display: inline-block; width: 14px; height: 14px; border-radius: 4px; vertical-align: -2px; border: 1px solid var(--line); }
      button.rs { height: 28px; padding: 0 10px; font-size: 12px; }
      .back { display: inline-flex; gap: 6px; align-items: center; color: var(--tx2); text-decoration: none; font-weight: 500; min-height: 32px; }
      .nmrow { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin-bottom: 8px; }
      .blk { display: block; margin-bottom: 12px; }
      .end { margin-top: 14px; }
      .fileerr { color: var(--bad); font-size: 12px; margin: 0; }
    `,
  ];
  /** theme under edit (route #/themes/<name>) */
  @property() selected = '';
  /** what the linked remote's firmware cannot show (from the app's link status) */
  @property({ attribute: false }) notices: string[] = [];
  @state() private fileError = '';
  @state() private nameDraft: string | null = null;
  private raf = 0;

  override disconnectedCallback(): void {
    cancelAnimationFrame(this.raf);
    super.disconnectedCallback();
  }

  private get cfg() {
    return this.store.config;
  }
  private edit(next: ReturnType<typeof selectTheme>, key?: string) {
    this.store.replace(next, key);
  }
  private userPage(): V2Page | null {
    return this.store.pages.find((p) => p.ok && p.page)?.page ?? null;
  }

  // ------------------------------------------------------------------ gallery
  private thumbs(th: RenderTheme) {
    const mine = this.userPage();
    const one = (p: V2Page, label: string, tiles?: Record<number, TileSample>) =>
      html`<svg viewBox="0 0 240 320" role="img" aria-label=${label}>${pageGroup(p, { noStatus: true, theme: th, ...(tiles ? { tiles } : {}) })}</svg>`;
    return html`<div class="thumbs">${one(SAMPLE_PAGE, `Sample page in ${th.name}`, SAMPLE_TILES)}${one(mine ?? SAMPLE_PAGE, mine ? `Your first page in ${th.name}` : `Sample page in ${th.name}`, mine ? undefined : SAMPLE_TILES)}</div>`;
  }
  private card(name: string, builtIn: boolean) {
    const th = themeNamed(this.cfg, name);
    const active = activeName(this.cfg) === name;
    const label = builtIn ? (PRESETS[name]?.name ?? name) : name;
    return html`<article class="card tc ${active ? 'on' : ''}" aria-label=${label}>
      <h3><span>${label}</span>${active ? statusChip('ok', 'In use', { check: true }) : statusChip('', builtIn ? 'Built in' : 'Yours')}</h3>
      ${this.thumbs(th.theme)}
      ${th.error ? html`<p class="fileerr" role="alert">${th.error}</p>` : nothing}
      <div class="acts">
        <button class="btn sm ${active ? '' : 'pri'}" aria-pressed=${active} aria-label="Use ${label}" ?disabled=${active} @click=${() => this.edit(selectTheme(this.cfg, name))}>${active ? 'In use' : 'Use'}</button>
        <button class="btn sm" aria-label="Duplicate ${label}" @click=${() => this.duplicate(name)}>Duplicate</button>
        ${builtIn ? nothing : html`<a class="btn sm" aria-label="Edit ${label}" href=${'#/themes/' + encodeURIComponent(name)}>Edit</a>
          <button class="btn sm" aria-label="Export ${label}" @click=${() => this.download(name)}>Export</button>`}
      </div>
    </article>`;
  }
  private duplicate(name: string) {
    const { config, name: nn } = duplicateTheme(this.cfg, name);
    this.edit(config);
    location.hash = '#/themes/' + encodeURIComponent(nn);
    this.store.toast(`Created "${nn}". Change its colours here, then switch to it with Use.`);
  }
  private download(name: string) {
    const url = URL.createObjectURL(new Blob([exportTheme(this.cfg, name)], { type: 'application/json' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `${name}.theme.json`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  private async onFile(e: Event) {
    const input = e.target as HTMLInputElement;
    const f = input.files?.[0];
    input.value = '';
    if (!f) return;
    try {
      const { config, name } = importTheme(this.cfg, await f.text());
      this.fileError = '';
      this.edit(config);
      this.store.toast(`Imported "${name}".`);
      location.hash = '#/themes/' + encodeURIComponent(name);
    } catch (err) {
      this.fileError = err instanceof Error ? err.message : String(err);
    }
  }

  // ------------------------------------------------------------------ editor
  private later(fn: () => void) {
    cancelAnimationFrame(this.raf);
    this.raf = requestAnimationFrame(fn);
  }
  private tokenRow(name: string, th: RenderTheme, t: TokenName) {
    const v = hexOf(th.tokens[t]);
    const own = overridesToken(this.cfg, name, t);
    const lab = TOKEN_LABEL[t];
    const set = (h: string, key = `th-${name}-${t}`) => this.edit(setToken(this.cfg, name, t, h), key);
    return html`<div class="tk" role="group" aria-label=${lab.name}>
      <input type="color" aria-label="${lab.name} colour" .value=${v.toLowerCase()} @input=${(e: Event) => { const h = (e.target as HTMLInputElement).value; this.later(() => set(h.toUpperCase())); }} />
      <div class="nm"><b>${lab.name} <span class="muted">${t.toUpperCase()}</span></b><span>${lab.where}</span></div>
      <input class="hx" type="text" maxlength="7" spellcheck="false" aria-label="${lab.name} hex" .value=${v} @change=${(e: Event) => { const el = e.target as HTMLInputElement; const h = parseHex(el.value); if (h) set(h); else el.value = v; }} />
      <button class="btn rs" ?hidden=${!own} aria-label="Reset ${lab.name}" @click=${() => this.edit(resetToken(this.cfg, name, t))}>Reset</button>
    </div>`;
  }
  private paletteCell(name: string, th: RenderTheme, slot: number) {
    const s = th.swatches[slot];
    if (!s) return nothing;
    const n = PALETTE_NAMES[slot] as string;
    const own = overridesSlot(this.cfg, name, slot);
    const set = (h: string) => this.edit(setPaletteFill(this.cfg, name, slot, h), `th-${name}-p${slot}`);
    return html`<div class="pc" role="group" aria-label=${n}>
      <div class="aa" style=${setStyle({ background: hex6(s.fill), color: hex6(s.ink) })}>Aa</div>
      <div><input type="color" aria-label="${n} fill" .value=${hexOf(s.fill).toLowerCase()} @input=${(e: Event) => { const h = (e.target as HTMLInputElement).value; this.later(() => set(h.toUpperCase())); }} /></div>
      <div class="a"><b>${n}</b><code>${hexOf(s.fill)}</code></div>
      <div class="a"><input class="hx" type="text" maxlength="7" spellcheck="false" aria-label="${n} hex" .value=${hexOf(s.fill)} @change=${(e: Event) => { const el = e.target as HTMLInputElement; const h = parseHex(el.value); if (h) set(h); else el.value = hexOf(s.fill); }} />
        <button class="btn rs" ?hidden=${!own} aria-label="Reset ${n}" @click=${() => this.edit(resetPaletteFill(this.cfg, name, slot))}>Reset</button></div>
    </div>`;
  }
  private async removeTheme(name: string) {
    const use = themeUsage(this.cfg, name);
    const parent = this.cfg.themes?.[name]?.extends ?? 'default';
    if (use.children.length) {
      this.store.toast(`"${name}" is the base of ${use.children.map((c) => `"${c}"`).join(', ')}. Change or delete those first.`, 'error');
      return;
    }
    const ok = await confirmAction(this, {
      title: `Delete "${name}"?`,
      body: use.active ? `The remote uses this theme now. Deleting it switches the remote to "${PRESETS[parent]?.name ?? parent}".` : 'This removes the theme from the config. Undo brings it back.',
      ok: use.active ? 'Switch and delete' : 'Delete',
      danger: true,
    });
    if (!ok) return;
    this.edit(deleteTheme(this.cfg, name));
    location.hash = '#/themes';
  }
  private rename(from: string) {
    const to = slugOf(this.nameDraft ?? from);
    this.nameDraft = null;
    if (to === from) return;
    if (PRESET_KEYS.includes(to) || userThemeNames(this.cfg).includes(to)) {
      this.store.toast(`There is already a theme called "${to}".`, 'error');
      return;
    }
    this.edit(renameTheme(this.cfg, from, to));
    location.hash = '#/themes/' + encodeURIComponent(to);
  }

  private editor(name: string) {
    const { theme: th, error } = themeNamed(this.cfg, name);
    const probs = themeProblems(th);
    const parent = this.cfg.themes?.[name]?.extends ?? 'default';
    const pages = this.store.pages.flatMap((p) => (p.ok && p.page ? [p.page] : [])).slice(0, 3);
    const first = pages[0] ?? SAMPLE_PAGE;
    const used = activeName(this.cfg) === name;
    const bad = this.nameDraft !== null && !/^[a-z0-9_]{1,31}$/.test(this.nameDraft);
    return html`<a class="back" href="#/themes">${icon('overview')} All themes</a>
      <div class="ed">
        <div class="card">
          <div class="nmrow">
            <label class="muted" for="tname">Name</label>
            <input id="tname" class="nm2" type="text" maxlength="31" spellcheck="false" .value=${this.nameDraft ?? name} aria-invalid=${bad ? 'true' : 'false'} aria-describedby="tname-h"
              @input=${(e: Event) => (this.nameDraft = (e.target as HTMLInputElement).value)} @change=${() => this.rename(name)} />
            ${statusChip(used ? 'ok' : '', used ? 'In use' : 'Not in use', { check: used })}
            <span class="muted" id="tname-h">Builds on ${PRESETS[parent]?.name ?? parent}. Letters a-z, digits and _ only.</span>
          </div>
          ${error ? html`<div class="banner bad" role="alert">${error}</div>` : nothing}
          ${probs.length
            ? html`<section class="banner ir blk" role="group" aria-label="Problems" id="theme-problems">
                <div class="nmrow">${statusChip('ir', `${probs.length} contrast problem${probs.length === 1 ? '' : 's'}`)}<span class="sp"></span>
                  <button class="btn sm" @click=${() => this.edit(fixAll(this.cfg, name))}>Fix all</button></div>
                ${probs.map((p) => html`<div class="prob"><span><b>${p.title}</b>: ${ratioText(p.ratio_x100)}:1, needs ${ratioText(p.min_x100)}:1. Nearest that works <i style=${setStyle({ background: hex6(p.fix) })}></i> ${hexOf(p.fix)}.</span>
                  <button class="btn sm" aria-label="Fix ${p.title}" @click=${() => this.edit(applyFix(this.cfg, name, p))}>Fix</button></div>`)}
              </section>`
            : html`<p>${statusChip('ok', 'Text is readable everywhere', { check: true })}</p>`}
          ${TOKEN_GROUPS.map((g) => html`<div class="grp"><h4>${g.title}</h4>${g.tokens.map((t) => this.tokenRow(name, th, t))}</div>`)}
          <div class="pal"><h4>Palette</h4><p class="muted">The 12 colours widgets can use. Text on each is chosen automatically.</p>
            <div class="row">${PALETTE_NAMES.map((_n, i) => this.paletteCell(name, th, i))}</div></div>
          <div class="acts end">
            <button class="btn" @click=${() => this.edit(selectTheme(this.cfg, name))} ?hidden=${used}>Use this theme</button>
            <button class="btn" @click=${() => this.duplicate(name)}>Duplicate</button>
            <button class="btn" @click=${() => this.download(name)}>Export</button>
            <button class="btn danger" @click=${() => void this.removeTheme(name)}>Delete</button>
          </div>
        </div>
        <div class="side" aria-label="Preview">
          <hh-mini-remote .page=${first} .theme=${th} label="Your remote in ${name}"></hh-mini-remote>
          <div class="th">${pages.slice(1).map((p) => svg`<svg viewBox="0 0 240 320" role="img" aria-label="Page in ${name}">${pageGroup(p, { noStatus: true, theme: th })}</svg>`)}${pages.length < 2 ? svg`<svg viewBox="0 0 240 320" role="img" aria-label="Sample page in ${name}">${pageGroup(SAMPLE_PAGE, { noStatus: true, theme: th, tiles: SAMPLE_TILES })}</svg>` : nothing}</div>
        </div>
      </div>`;
  }

  protected override render() {
    const names = userThemeNames(this.cfg);
    const sel = this.selected && names.includes(this.selected) ? this.selected : '';
    return html`<header class="top"><div><h2 class="t">Themes</h2>
        <span class="muted">One theme styles the whole remote. Pick a built-in one, or duplicate it and make it yours.</span></div>
        <button class="btn" @click=${() => this.renderRoot.querySelector<HTMLInputElement>('#imp')?.click()}>${icon('upload')} Import theme</button>
        <input id="imp" type="file" accept=".json,application/json" hidden aria-label="Import a theme file" @change=${(e: Event) => void this.onFile(e)} />
      </header>
      ${this.fileError ? html`<div class="banner bad" role="alert">${this.fileError}</div>` : nothing}
      ${themeNotice(this.notices)}
      ${this.selected && !sel ? html`<div class="banner ir" role="status">There is no theme called "${this.selected}". <a href="#/themes">Show all themes</a></div>` : nothing}
      ${sel
        ? this.editor(sel)
        : html`<div class="gal" role="list" aria-label="Themes">${[...PRESET_KEYS.map((k) => [k, true] as const), ...names.map((n) => [n, false] as const)].map(([n, b]) => html`<div role="listitem">${this.card(n, b)}</div>`)}</div>`}`;
  }
}
