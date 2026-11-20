import { html, nothing, type TemplateResult } from 'lit';
import { classMap } from 'lit/directives/class-map.js';
import { setStyle } from '../lib/set-style';
import type { Store } from '../lib/store';
import type { Problem } from '../lib/problems';
import { icon } from '../lib/icons';
import { degradeText } from '../lib/theme-model';

export function meter(label: string, text: string, value: number, max: number, hint?: string, tip?: string): TemplateResult {
  const pct = max > 0 ? Math.min(100, (value / max) * 100) : 0;
  const ratio = max > 0 ? value / max : 0;
  const level = ratio >= 1 ? 'full' : ratio >= 0.8 ? 'warn' : '';
  const over = level === 'full' && hint ? ` ${hint}` : '';
  return html`<div class="meter" role="group" aria-label="${label}: ${text}${over}" title=${tip ?? ''}>
    <span><b>${label}</b> ${text}${over ? html` <em class="over">${hint}</em>` : nothing}</span>
    <div class="bar"><i class=${classMap({ warn: level === 'warn', full: level === 'full' })} style=${setStyle({ width: `${pct}%`, 'min-width': pct > 0 ? '2px' : '0' })}></i></div>
  </div>`;
}

const net = (bytes: number, overhead?: number | null) => Math.max(0, bytes - (overhead ?? 0));
/** KiB below 1 MiB (one decimal under 10), MiB above; rounded up for a used size and down for a limit, so "over" never reads as equal. */
export function sizeText(bytes: number, round: 'up' | 'down'): string {
  const r = round === 'up' ? Math.ceil : Math.floor;
  if (bytes >= 1048576) return `${r((bytes / 1048576) * 10) / 10} MiB`;
  const kib = bytes / 1024;
  return `${kib < 10 ? r(kib * 10) / 10 : r(kib)} KiB`;
}

/** The one place label and bar of the "Config size" meter come from: both are the net figures (the hub's fixed load
 * overhead left out), so an empty config reads and draws as about 0. Net used > net available exactly when used > budget. */
export function configSizeFigures(bytes: number, budget: number, overhead?: number | null) {
  const used = net(bytes, overhead);
  const max = net(budget, overhead);
  return { used, max, text: `${sizeText(used, 'up')} of ${sizeText(max, 'down')} on this hub` };
}
export function configSizeMeter(bytes: number, budget: number, overhead?: number | null): TemplateResult {
  const f = configSizeFigures(bytes, budget, overhead);
  return meter('Config size', f.text, f.used, f.max, 'over, remove commands', 'The hub keeps a fixed part of its budget to load any config; it is left out here, so an empty config reads about 0 and the right side is what you can fill.');
}

/** The draft meters, fed by the preview's `usage`. A current hub has one: "Config size X of Y KiB on this hub" (its memory
 * budget). Pages are the remote's. */
export function meters(store: Store): TemplateResult {
  const u = store.usage;
  if (!u) return html``;
  return html`
    ${configSizeMeter(u.config_bytes, u.config_budget, u.config_overhead)}
    ${meter('Pages', `${u.pages} / ${u.page_limit}`, u.pages, u.page_limit)}
  `;
}

export function ago(sec: number | null, nowMs = Date.now()): string {
  if (sec == null) return '';
  const d = Math.max(0, Math.round(nowMs / 1000 - sec));
  if (d < 60) return 'just now';
  if (d < 3600) return `${Math.floor(d / 60)} min ago`;
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`;
  return new Date(sec * 1000).toLocaleDateString();
}
/** Milliseconds until `ago(sec)` reads differently (so a view can re-render once then, instead of on a clock). */
export function agoChangeIn(sec: number | null, nowMs = Date.now()): number | null {
  if (sec == null) return null;
  const d = Math.max(0, nowMs / 1000 - sec);
  if (d >= 86400) return null;
  const unit = d < 60 ? 60 : d < 3600 ? 60 : 3600;
  const next = d < 60 ? 60 : (Math.floor(d / unit) + 1) * unit;
  return Math.max(1000, Math.ceil((next - d) * 1000));
}
export function when(sec: number): string {
  return new Date(sec * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
}

/** The errors of one device or activity, each in words with the backend's own message. */
export function problemList(list: Problem[], onOpen?: (p: Problem) => void): TemplateResult | typeof nothing {
  if (!list.length) return nothing;
  return html`<section class="card problems" role="alert" aria-label="Problems">
    <h2>${list.length} problem${list.length === 1 ? '' : 's'} to fix before applying</h2>
    <ul>${list.map((p) => html`<li id=${'f-' + p.path.join('.').replace(/[^A-Za-z0-9_.-]/g, '_')}><b>${p.label}</b> <span>${p.msg}</span>${p.open && onOpen ? html`<button class="btn sm" @click=${() => onOpen(p)}>Open</button>` : nothing}</li>`)}</ul>
  </section>`;
}

/** The one status chip: tone ok (green) / bad (red) / ir (amber) / '' (neutral), sentence-case text, optional check icon.
 * Header link state, "Valid", and the History row status all come from here, so they cannot drift apart. */
export type ChipTone = 'ok' | 'bad' | 'ir' | '';
export function statusChip(tone: ChipTone, label: string, o: { check?: boolean; title?: string; id?: string } = {}): TemplateResult {
  return html`<span class="chip ${tone}" id=${o.id ?? nothing} title=${o.title ?? nothing}>${o.check ? icon('check') : nothing}${label}</span>`;
}

/** The one non-blocking notice for a remote whose firmware lacks colours or custom themes (Themes view and apply dialog). */
export function themeNotice(msgs: readonly string[] | undefined): TemplateResult | typeof nothing {
  const text = degradeText(msgs);
  if (!text) return nothing;
  return html`<div class="banner ir" role="status" id="theme-notice"><span>${icon('alert')} ${text}</span>
    <details><summary>What changes</summary><ul>${(msgs ?? []).map((m) => html`<li>${m}</li>`)}</ul></details></div>`;
}
