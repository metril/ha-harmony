import { html, nothing, type TemplateResult } from 'lit';
import { classMap } from 'lit/directives/class-map.js';
import { setStyle } from '../lib/set-style';
import type { Store } from '../lib/store';
import { LIMITS, counts } from '../lib/model';
import type { Problem } from '../lib/problems';

export function meter(label: string, text: string, value: number, max: number, hint?: string, tip?: string): TemplateResult {
  const pct = max > 0 ? Math.min(100, (value / max) * 100) : 0;
  const ratio = max > 0 ? value / max : 0;
  const level = ratio >= 1 ? 'full' : ratio >= 0.8 ? 'warn' : '';
  const over = level === 'full' && hint ? ` ${hint}` : '';
  return html`<div class="meter" role="group" aria-label="${label}: ${text}${over}" title=${tip ?? ''}>
    <span><b>${label}</b> ${text}${over ? html` <em class="over">${hint}</em>` : nothing}</span>
    <div class="bar"><i class=${classMap({ warn: level === 'warn', full: level === 'full' })} style=${setStyle({ width: `${pct}%` })}></i></div>
  </div>`;
}

/** The draft meters, fed by the preview's `usage`. A current hub has one: "Config size X of Y KiB on this hub" (its memory
 * budget); old firmware has fixed limits: config KiB, IR pool %, commands. Pages are the remote's. */
export function meters(store: Store): TemplateResult {
  const u = store.usage;
  const cmds = u?.commands ?? counts(store.config).commands;
  const cmax = u?.commands_max ?? LIMITS.commands;
  if (u && u.config_budget != null && u.config_bytes != null)
    return html`
      ${meter('Config size', `${Math.ceil(u.config_bytes / 1024)} of ${Math.floor(u.config_budget / 1024)} KiB on this hub`, u.config_bytes, u.config_budget, 'over, remove commands', 'The hub reserves a fixed part of this budget to load any config, so even a tiny config shows a large size.')}
      ${meter('Pages', `${u.pages} / ${u.page_limit}`, u.pages, u.page_limit)}
    `;
  return html`
    ${u && u.json_max != null
      ? meter('Config', `${(u.json_bytes / 1024).toFixed(1)} / ${(u.json_max / 1024).toFixed(0)} KiB`, u.json_bytes, u.json_max)
      : nothing}
    ${u && u.pool_max != null ? meter('IR pool', `${Math.round((u.pool_words / u.pool_max) * 100)}%`, u.pool_words, u.pool_max) : nothing}
    ${u ? meter('Pages', `${u.pages} / ${u.page_limit}`, u.pages, u.page_limit) : nothing}
    ${Number.isFinite(cmax) ? meter('Commands', `${cmds} / ${cmax}`, cmds, cmax) : nothing}
  `;
}

export function ago(sec: number | null, nowMs = Date.now()): string {
  if (sec == null) return '';
  const d = Math.max(0, Math.round(nowMs / 1000 - sec));
  if (d < 5) return 'just now';
  if (d < 60) return `${d} s ago`;
  if (d < 3600) return `${Math.floor(d / 60)} min ago`;
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`;
  return new Date(sec * 1000).toLocaleDateString();
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
