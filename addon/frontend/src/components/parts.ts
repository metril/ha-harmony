import { html, nothing, type TemplateResult } from 'lit';
import { classMap } from 'lit/directives/class-map.js';
import { setStyle } from '../lib/set-style';
import type { Store } from '../lib/store';
import { LIMITS, counts } from '../lib/model';

export function meter(label: string, text: string, value: number, max: number): TemplateResult {
  const pct = max > 0 ? Math.min(100, (value / max) * 100) : 0;
  const ratio = max > 0 ? value / max : 0;
  const level = ratio >= 1 ? 'full' : ratio >= 0.8 ? 'warn' : '';
  return html`<div class="meter" role="group" aria-label="${label}: ${text}">
    <span><b>${label}</b> ${text}</span>
    <div class="bar"><i class=${classMap({ warn: level === 'warn', full: level === 'full' })} style=${setStyle({ width: `${pct}%` })}></i></div>
  </div>`;
}

/** The three draft meters, fed by the preview's `usage`: config KiB, IR pool %, commands. */
export function meters(store: Store): TemplateResult {
  const u = store.usage;
  const cmds = u?.commands ?? counts(store.config).commands;
  const cmax = u?.commands_max ?? LIMITS.commands;
  return html`
    ${u
      ? meter('Config', `${(u.json_bytes / 1024).toFixed(1)} / ${(u.json_max / 1024).toFixed(0)} KiB`, u.json_bytes, u.json_max)
      : nothing}
    ${u ? meter('IR pool', `${Math.round((u.pool_words / u.pool_max) * 100)}%`, u.pool_words, u.pool_max) : nothing}
    ${u ? meter('Pages', `${u.pages} / ${u.page_limit}`, u.pages, u.page_limit) : nothing}
    ${meter('Commands', `${cmds} / ${cmax}`, cmds, cmax)}
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
