import { css, html, nothing, svg } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { V2Page, V2Widget } from '../lib/api';
import { layoutPage, pageGroup, primTemplate, type Prim, type TileSample } from '../lib/page-renderer';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';
import { setStyle } from '../lib/set-style';
import { PALETTE_NAMES, TOKEN_NAMES, css as hex6, type TokenName } from '../lib/themes';
import {
  TOKEN_GROUPS, TOKEN_LABEL, applyFix, deleteTheme, duplicateTheme, exportTheme, fixAll, importTheme, overridesSlot, overridesToken, ratioText, renameTheme,
  resetPaletteFill, resetToken, selectTheme, setPaletteFill, setToken, slugOf, themeProblems, themeUsage, type ThemeProblem,
} from '../lib/theme-edit';
import { PRESETS, PRESET_KEYS, activeName, hexOf, parseHex, themeNamed, userThemeNames, type RenderTheme } from '../lib/theme-model';
import { StoreElement, confirmAction } from '../components/base';
import { statusChip, themeNotice } from '../components/parts';

const sw = (id: number, kind: V2Widget['kind'], x: number, y: number, w: number, label: string, extra: Partial<V2Widget> = {}): V2Widget => ({
  kind, id, x, y, w, h: 1, label, role: 'default', shape: 'card', icon_only: false, icon: 0, bind: null, ...extra,
});
/** A page that exercises every token: roles, the running activity, palette and free colours, tile states; the palette strip and a toast are drawn on top. */
export const SAMPLE_PAGE: V2Page = {
  version: 2, id: 0, cols: 4, rows: 6, gen: 0, total: 1, kind: 'normal', back: false, seq: { idx: 1, cnt: 2 }, act: 0, title: 'Sample', theme: 0,
  widgets: [
    sw(0, 'label', 0, 0, 4, 'Living room', { color: 'blue' }),
    sw(1, 'button', 0, 1, 1, 'Main', { role: 'primary' }),
    sw(2, 'button', 1, 1, 1, 'Side', { role: 'secondary' }),
    sw(3, 'button', 2, 1, 1, 'Next', { role: 'nav' }),
    sw(4, 'button', 3, 1, 1, 'Plain'),
    sw(5, 'button', 0, 2, 1, 'Active', { bind: 0 }),
    sw(6, 'button', 1, 2, 1, 'Off', { role: 'danger' }),
    sw(7, 'button', 2, 2, 1, 'Red', { color: 'red' }),
    sw(8, 'button', 3, 2, 1, 'Mine', { color: '#E0B040' }),
    sw(9, 'tile', 0, 3, 1, 'On'),
    sw(10, 'tile', 1, 3, 1, 'Off'),
    sw(11, 'tile', 2, 3, 1, 'Gone'),
    sw(12, 'tile', 3, 3, 1, 'Stale'),
  ],
};
export const SAMPLE_TILES: Record<number, TileSample> = {
  9: { state: 'on', text: '1' },
  10: { state: 'off', text: '0' },
  11: { state: 'unavailable', text: 'n/a' },
  12: { state: 'off', text: '5', stale: true },
};
const num = (c: string | undefined): number | null => (c && /^#[0-9a-f]{6}$/i.test(c) ? parseInt(c.slice(1), 16) : null);
/** The theme's own overlay on the sample screen: the 12 palette fills and an alert toast. */
function overlay(th: RenderTheme): Prim[] {
  const out: Prim[] = [];
  th.swatches.forEach((s, i) => {
    const x = 12 + (i % 6) * 36;
    const y = 232 + Math.floor(i / 6) * 24;
    out.push({ t: 'rect', x, y, w: 32, h: 20, r: 5, fill: hex6(s.fill), wi: 101 + i });
    out.push({ t: 'text', x: x + 16, y: y + 14, s: 'Aa', size: 11, fill: hex6(s.ink), anchor: 'middle', weight: 600, wi: 101 + i });
  });
  out.push({ t: 'rect', x: 12, y: 282, w: 216, h: 28, r: 14, fill: hex6(th.tokens.bad), wi: 100 });
  out.push({ t: 'text', x: 120, y: 301, s: 'Alert: power off', size: 13, fill: hex6(th.tokens.onbad), anchor: 'middle', weight: 500, wi: 100 });
  return out;
}
function bbox(q: Prim): [number, number, number, number] | null {
  switch (q.t) {
    case 'rect': return [q.x, q.y, q.w, q.h];
    case 'circle': return [q.x - q.r, q.y - q.r, 2 * q.r, 2 * q.r];
    case 'icon': return [q.x, q.y, q.size, q.size];
    case 'text': {
      const w = Array.from(q.s).length * q.size * 0.54;
      return [q.anchor === 'middle' ? q.x - w / 2 : q.anchor === 'end' ? q.x - w : q.x, q.y - q.size * 0.8, w, q.size];
    }
    default: return null;
  }
}
function colours(q: Prim): number[] {
  const l = q.t === 'rect' || q.t === 'circle' ? [num(q.fill), num(q.stroke)] : q.t === 'text' ? [num(q.fill)] : q.t === 'icon' ? [num(q.color)] : [num(q.stroke)];
  return l.filter((v): v is number => v !== null);
}

@customElement('hh-themes')
export class HhThemes extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; padding-bottom: 120px; }
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
      .ed { display: grid; grid-template-columns: minmax(0, 1fr) 380px; gap: 16px; align-items: start; }
      @media (max-width: 900px) { .ed { grid-template-columns: minmax(0, 1fr); } }
      .side { position: sticky; top: 144px; display: grid; gap: 8px; z-index: 2; background: var(--bg); padding-bottom: 4px; }
      @media (max-width: 900px) { .side { top: 48px; order: -1; } }
      .side .sbar { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
      .side .tabs { display: flex; gap: 4px; }
      .side .tabs button { height: 28px; padding: 0 12px; border-radius: 14px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 600; font-size: 12px; cursor: pointer; }
      .side .tabs button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      svg.pv { width: 100%; height: auto; max-height: 560px; border-radius: 10px; box-shadow: 0 0 0 1px var(--line); display: block; cursor: pointer; touch-action: manipulation; }
      @media (max-width: 900px) { svg.pv { max-height: 40vh; } }
      svg.pv [data-i]:focus-visible { outline: none; }
      .pager { display: flex; gap: 8px; align-items: center; justify-content: center; font-size: 12px; color: var(--tx2); }
      .hint { font-size: 12px; color: var(--tx3); margin: 0; }
      .grp { display: grid; gap: 6px; margin-bottom: 14px; }
      .grp h3, .pal h3 { margin: 0; font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; }
      .tk { display: grid; grid-template-columns: 40px minmax(0, 1fr) auto; gap: 2px 8px; align-items: center; padding: 6px 4px; border-top: 1px solid var(--line); border-radius: 4px; }
      .tk.hl, .pc.hl { background: var(--accs); outline: 2px solid var(--acc); }
      .tk .nm { display: grid; min-width: 0; }
      .tk .nm b { font-size: 13px; font-weight: 600; }
      .tk .nm span { font-size: 11px; color: var(--tx3); }
      .tk .pi { grid-column: 2 / -1; display: flex; gap: 6px; align-items: center; flex-wrap: wrap; font-size: 12px; color: var(--ir); }
      @media (max-width: 520px) { .tk .nm span { display: none; } }
      .tk .nm button.rs { justify-self: start; margin-top: 2px; }
      input[type="color"] { width: 40px; height: 34px; padding: 2px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); cursor: pointer; }
      input.hx, input.nm2 { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; }
      input.hx { width: 10.5ch; font-family: var(--mono); }
      input.nm2 { width: 100%; max-width: 280px; }
      input[aria-invalid="true"] { border-color: var(--bad); }
      .pal .row { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; margin-top: 6px; }
      .pc { display: grid; grid-template-columns: 40px minmax(0, 1fr); gap: 4px 8px; align-items: center; padding: 6px; border: 1px solid var(--line); border-radius: var(--r1); }
      .pc .a { grid-column: 1 / -1; display: flex; gap: 6px; align-items: center; justify-content: space-between; flex-wrap: wrap; }
      .pc .a b { font-size: 12px; text-transform: capitalize; }
      .pc .a code { font-size: 11px; color: var(--tx3); }
      .pc .aa { height: 34px; border-radius: 7px; display: grid; place-items: center; font-weight: 650; font-size: 13px; }
      .prob { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; padding: 6px 0; border-top: 1px solid var(--line); }
      .prob span { flex: 1 1 200px; min-width: 0; }
      .prob i { display: inline-block; width: 14px; height: 14px; border-radius: 4px; vertical-align: -2px; border: 1px solid var(--line); }
      button.rs { height: 28px; padding: 0 10px; font-size: 12px; min-height: 28px; }
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
  @state() private tab: 'sample' | 'pages' = 'sample';
  @state() private pageIdx = 0;
  /** tokens ('bg', ...) and palette slots ('p3') that colour the element last clicked in the preview */
  @state() private hl = new Set<string>();
  /** colour of the token row under the pointer: its uses are outlined in the preview */
  @state() private hover: number | null = null;
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
  private userPages(): V2Page[] {
    return this.store.pages.flatMap((p) => (p.ok && p.page ? [p.page] : []));
  }

  // ------------------------------------------------------------------ gallery
  private thumbs(th: RenderTheme) {
    const mine = this.userPages()[0];
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
    this.tab = 'sample';
    location.hash = '#/themes/' + encodeURIComponent(nn);
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
  private rowEvents(value: number) {
    return { enter: () => (this.hover = value), leave: () => (this.hover = null) };
  }
  private problemLines(ps: ThemeProblem[], name: string) {
    return ps.map((p) => html`<div class="pi" role="note">${statusChip('ir', `${ratioText(p.ratio_x100)}:1`)}<span>${p.target.kind === 'token' ? `on ${(TOKEN_LABEL[p.surface as TokenName]?.name ?? p.surface).toLowerCase()}` : `text on ${p.surface}`}, needs ${ratioText(p.min_x100)}:1</span>
      <button class="btn rs" aria-label="Fix ${p.title}" @click=${() => this.edit(applyFix(this.cfg, name, p))}>Fix</button></div>`);
  }
  private tokenRow(name: string, th: RenderTheme, t: TokenName, probs: ThemeProblem[]) {
    const v = hexOf(th.tokens[t]);
    const own = overridesToken(this.cfg, name, t);
    const lab = TOKEN_LABEL[t];
    const set = (h: string, key = `th-${name}-${t}`) => this.edit(setToken(this.cfg, name, t, h), key);
    const ev = this.rowEvents(th.tokens[t]);
    return html`<div class="tk ${this.hl.has(t) ? 'hl' : ''}" role="group" aria-label=${lab.name} data-token=${t} @mouseenter=${ev.enter} @mouseleave=${ev.leave} @focusin=${ev.enter} @focusout=${ev.leave}>
      <input type="color" aria-label="${lab.name} colour" .value=${v.toLowerCase()} @input=${(e: Event) => { const h = (e.target as HTMLInputElement).value; this.later(() => set(h.toUpperCase())); }} />
      <div class="nm"><b>${lab.name} <span class="muted">${t.toUpperCase()}</span></b><span>${lab.where}</span>
        <button class="btn rs" ?hidden=${!own} aria-label="Reset ${lab.name}" title="Back to the value of ${this.cfg.themes?.[name]?.extends ?? 'default'}" @click=${() => this.edit(resetToken(this.cfg, name, t))}>Reset</button></div>
      <input class="hx" type="text" maxlength="7" spellcheck="false" aria-label="${lab.name} hex" .value=${v} @change=${(e: Event) => { const el = e.target as HTMLInputElement; const h = parseHex(el.value); if (h) set(h); else el.value = v; }} />
      ${this.problemLines(probs.filter((p) => p.target.kind === 'token' && p.target.token === t), name)}
    </div>`;
  }
  private paletteCell(name: string, th: RenderTheme, slot: number, probs: ThemeProblem[]) {
    const s = th.swatches[slot];
    if (!s) return nothing;
    const n = PALETTE_NAMES[slot] as string;
    const own = overridesSlot(this.cfg, name, slot);
    const set = (h: string) => this.edit(setPaletteFill(this.cfg, name, slot, h), `th-${name}-p${slot}`);
    const ev = this.rowEvents(s.fill);
    return html`<div class="pc ${this.hl.has('p' + slot) ? 'hl' : ''}" role="group" aria-label=${n} @mouseenter=${ev.enter} @mouseleave=${ev.leave} @focusin=${ev.enter} @focusout=${ev.leave}>
      <div class="aa" style=${setStyle({ background: hex6(s.fill), color: hex6(s.ink) })}>Aa</div>
      <div><input type="color" aria-label="${n} fill" .value=${hexOf(s.fill).toLowerCase()} @input=${(e: Event) => { const h = (e.target as HTMLInputElement).value; this.later(() => set(h.toUpperCase())); }} /></div>
      <div class="a"><b>${n}</b><code>${hexOf(s.fill)}</code></div>
      <div class="a"><input class="hx" type="text" maxlength="7" spellcheck="false" aria-label="${n} hex" .value=${hexOf(s.fill)} @change=${(e: Event) => { const el = e.target as HTMLInputElement; const h = parseHex(el.value); if (h) set(h); else el.value = hexOf(s.fill); }} />
        <button class="btn rs" ?hidden=${!own} aria-label="Reset ${n}" @click=${() => this.edit(resetPaletteFill(this.cfg, name, slot))}>Reset</button></div>
      ${probs.filter((p) => p.target.kind === 'slot' && p.target.slot === slot).map((p) => html`<div class="a">${statusChip('ir', `text ${ratioText(p.ratio_x100)}:1`)}<button class="btn rs" aria-label="Fix ${p.title}" @click=${() => this.edit(applyFix(this.cfg, name, p))}>Fix</button></div>`)}
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

  /** The screen at full size in the theme under edit: the sample page (every token) or one of the user's pages. Click an element to find its tokens. */
  private preview(th: RenderTheme) {
    const pages = this.userPages();
    const useMine = this.tab === 'pages' && pages.length > 0;
    const idx = Math.min(this.pageIdx, Math.max(0, pages.length - 1));
    const page = useMine ? (pages[idx] as V2Page) : SAMPLE_PAGE;
    const prims = [...layoutPage(page, useMine ? { theme: th } : { theme: th, tiles: SAMPLE_TILES, activeAct: 0 }).prims, ...(useMine ? [] : overlay(th))];
    const marks: Prim[] = [];
    if (this.hover !== null)
      for (const q of prims) {
        const b = bbox(q);
        if (b && colours(q).includes(this.hover) && !(q.t === 'rect' && q.w >= 240 && q.h >= 320 && q.x === 0)) marks.push({ t: 'rect', x: b[0] - 1, y: b[1] - 1, w: b[2] + 2, h: b[3] + 2, r: 3, fill: 'none', stroke: '#ff2bd6', sw: 1.5, dash: true });
      }
    return html`<svg class="pv" viewBox="0 0 240 320" role="group" aria-label="Preview of ${th.name}" @click=${(e: Event) => this.pick(e, prims, th)}>
      <g font-family="system-ui, -apple-system, 'Segoe UI', Roboto, Ubuntu, sans-serif">${prims.map((q, i) => svg`<g data-i=${i}>${primTemplate(q)}</g>`)}</g>
      ${marks.map((m) => primTemplate(m))}
    </svg>`;
  }
  private pick(e: Event, prims: Prim[], th: RenderTheme) {
    const g = (e.target as Element).closest('[data-i]');
    const q = prims[Number(g?.getAttribute('data-i'))];
    if (!q) return;
    const group = q.wi === undefined ? [q] : prims.filter((p) => p.wi === q.wi);
    const vals = new Set(group.flatMap(colours));
    const hl = new Set<string>();
    for (const t of TOKEN_NAMES) if (vals.has(th.tokens[t])) hl.add(t);
    th.swatches.forEach((s, i) => vals.has(s.fill) && hl.add('p' + i));
    this.hl = hl;
    const first = [...hl][0];
    if (first) this.renderRoot.querySelector(`[data-token="${first}"]`)?.scrollIntoView({ block: 'center', behavior: 'smooth' });
  }

  private editor(name: string) {
    const { theme: th, error } = themeNamed(this.cfg, name);
    const probs = themeProblems(th);
    const parent = this.cfg.themes?.[name]?.extends ?? 'default';
    const pages = this.userPages();
    const used = activeName(this.cfg) === name;
    const bad = this.nameDraft !== null && !/^[a-z0-9_]{1,31}$/.test(this.nameDraft);
    const idx = Math.min(this.pageIdx, Math.max(0, pages.length - 1));
    return html`<a class="back" href="#/themes">${icon('overview')} All themes</a>
      <div class="ed">
        <div class="card">
          <div class="nmrow">
            <label class="muted" for="tname">Name</label>
            <input id="tname" class="nm2" type="text" maxlength="31" spellcheck="false" .value=${this.nameDraft ?? name} aria-invalid=${bad ? 'true' : 'false'} aria-describedby="tname-h"
              @input=${(e: Event) => (this.nameDraft = (e.target as HTMLInputElement).value)} @change=${() => this.rename(name)} />
            ${statusChip(used ? 'ok' : '', used ? 'In use' : 'Not in use', { check: used })}
            <span class="muted" id="tname-h">Builds on ${PRESETS[parent]?.name ?? parent}. Name: letters a-z (lower case), digits, underscore.</span>
          </div>
          ${error ? html`<div class="banner bad" role="alert">${error}</div>` : nothing}
          ${probs.length
            ? html`<section class="banner ir blk" role="group" aria-label="Problems" id="theme-problems">
                ${probs.map((p) => html`<div class="prob"><span><b>${p.title}</b>: ${ratioText(p.ratio_x100)}:1, needs ${ratioText(p.min_x100)}:1. Nearest that works <i style=${setStyle({ background: hex6(p.fix) })}></i> ${hexOf(p.fix)}.</span>
                  <button class="btn sm" aria-label="Fix ${p.title}" @click=${() => this.edit(applyFix(this.cfg, name, p))}>Fix</button></div>`)}
              </section>`
            : html`<p>${statusChip('ok', 'Text is readable everywhere', { check: true })}</p>`}
          ${TOKEN_GROUPS.map((g) => html`<div class="grp"><h3>${g.title}</h3>${g.tokens.map((t) => this.tokenRow(name, th, t, probs))}</div>`)}
          <div class="pal"><h3>Palette</h3><p class="muted">The 12 colours widgets can use. Text on each is chosen automatically.</p>
            <div class="row">${PALETTE_NAMES.map((_n, i) => this.paletteCell(name, th, i, probs))}</div></div>
          <div class="acts end">
            <button class="btn" @click=${() => this.edit(selectTheme(this.cfg, name))} ?hidden=${used}>Use this theme</button>
            <button class="btn" @click=${() => this.duplicate(name)}>Duplicate</button>
            <button class="btn" @click=${() => this.download(name)}>Export</button>
            <button class="btn danger" @click=${() => void this.removeTheme(name)}>Delete</button>
          </div>
        </div>
        <div class="side" aria-label="Preview">
          <div class="sbar">
            ${probs.length ? statusChip('ir', `${probs.length} contrast problem${probs.length === 1 ? '' : 's'}`, { id: 'prob-chip' }) : statusChip('ok', 'Readable', { check: true })}
            ${probs.length ? html`<button class="btn sm" @click=${() => this.edit(fixAll(this.cfg, name))}>Fix all</button>` : nothing}
            <span class="sp"></span>
            <div class="tabs" role="group" aria-label="Preview page">
              <button aria-pressed=${this.tab === 'sample'} @click=${() => (this.tab = 'sample')}>Sample</button>
              <button aria-pressed=${this.tab === 'pages'} ?disabled=${!pages.length} @click=${() => (this.tab = 'pages')}>Your pages</button>
            </div>
          </div>
          ${this.preview(th)}
          ${this.tab === 'pages' && pages.length
            ? html`<div class="pager"><button class="btn sm" aria-label="Previous page" ?disabled=${idx === 0} @click=${() => (this.pageIdx = idx - 1)}>‹</button>
                <span>${pages[idx]?.title || 'Page'} · ${idx + 1} of ${pages.length}</span>
                <button class="btn sm" aria-label="Next page" ?disabled=${idx >= pages.length - 1} @click=${() => (this.pageIdx = idx + 1)}>›</button></div>`
            : nothing}
          <p class="hint">Click anything on the screen to find the colours behind it. Point at a colour to outline where it is used.</p>
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
