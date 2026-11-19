// Themes and widget colours as the page renderer needs them: the active theme of a config (preset or user theme, resolved
// with `extends`) and the remote's widget colour rules (remote/app/ui.c widget_colors). Editing lives in theme-edit.ts (lazy).
import type { Config } from './model';
import { PALETTE_NAMES, THEMES, css, type Swatch, type Theme } from './themes';
import { ink as inkOf, meets, needsOutline, pressed as pressedOf, resolveTheme, snap565, type Tokens, type UserTheme } from './theme-math';

export const PRESETS: Record<string, Theme> = Object.fromEntries(THEMES.map((t) => [t.key, t]));
export const PRESET_KEYS = THEMES.map((t) => t.key);
export const SLUG = /^[a-z0-9_]{1,31}$/;

/** What the renderer needs: 13 tokens and 12 derived swatches (fill, ink, pressed). */
export interface RenderTheme {
  name: string;
  /** the preset it stands on (itself for a preset) */
  base: string;
  tokens: Tokens;
  swatches: Swatch[];
}

export function deriveSwatches(tokens: Tokens, fills: readonly number[]): Swatch[] {
  return fills.map((f, i) => {
    const i2 = inkOf(f, tokens.tx, tokens.bg);
    return { name: PALETTE_NAMES[i] ?? 'red', fill: f, ink: i2, pressed: pressedOf(f, i2) };
  });
}
export function presetTheme(key: string): RenderTheme {
  const p = PRESETS[key] ?? (PRESETS['default'] as Theme);
  return { name: p.key, base: p.key, tokens: { ...p.tokens }, swatches: p.palette.map((s) => ({ ...s })) };
}
export const DEFAULT_THEME: RenderTheme = presetTheme('default');

export const userThemes = (cfg: Config): Record<string, UserTheme> => (cfg['themes'] ?? {}) as Record<string, UserTheme>;
export const userThemeNames = (cfg: Config): string[] => Object.keys((cfg['themes'] as Record<string, unknown> | undefined) ?? {});
export const activeName = (cfg: Config): string => (typeof cfg['theme'] === 'string' && cfg['theme'] ? cfg['theme'] : 'default');

/** The preset a theme name stands on ("default" on a broken chain). */
export function baseOf(name: string, themes: Record<string, UserTheme>): string {
  let cur = name;
  for (let i = 0; i < 6; i++) {
    if (cur in PRESETS && !(cur in themes)) return cur;
    const next = themes[cur]?.extends;
    if (typeof next !== 'string') return 'default';
    cur = next;
  }
  return 'default';
}

/** A named theme of the config, resolved; a broken one (loops, unknown parent) falls back to the default look with `error`. */
export function themeNamed(cfg: Config, name: string): { theme: RenderTheme; error?: string } {
  if (name in PRESETS) return { theme: presetTheme(name) };
  const themes = userThemes(cfg);
  try {
    const r = resolveTheme(name, themes, PRESETS);
    return { theme: { name, base: baseOf(name, themes), tokens: r.tokens, swatches: deriveSwatches(r.tokens, r.fills) } };
  } catch (e) {
    return { theme: { ...presetTheme('default'), name }, error: e instanceof Error ? e.message : String(e) };
  }
}
export const activeTheme = (cfg: Config): { theme: RenderTheme; error?: string } => themeNamed(cfg, activeName(cfg));

// ------------------------------------------------------------------ colours
export const HEX = /^#[0-9a-fA-F]{6}$/;
export const isFree = (c: unknown): c is string => typeof c === 'string' && c.startsWith('#');
export const hexOf = (n: number): string => css(n).toUpperCase();
export const rgbOf = (h: string): number => parseInt(h.slice(1), 16);
/** `#abc` / `abcdef` / `#ABCDEF` -> `#ABCDEF`, or null */
export function parseHex(s: string): string | null {
  let t = s.trim().replace(/^#/, '');
  if (/^[0-9a-fA-F]{3}$/.test(t)) t = t.replace(/./g, (c) => c + c);
  return /^[0-9a-fA-F]{6}$/.test(t) ? `#${t.toUpperCase()}` : null;
}

export interface Resolved {
  fill: number;
  ink: number;
  pressed: number;
  outline: boolean;
}
/** A widget colour (palette name or `#RRGGBB`) in a theme: palette entries as the theme defines them, free colours snapped with derived ink. */
export function resolveColour(spec: string | null | undefined, th: RenderTheme): Resolved | null {
  if (!spec) return null;
  if (spec.startsWith('#')) {
    if (!HEX.test(spec)) return null;
    const fill = snap565(rgbOf(spec));
    const i = inkOf(fill, th.tokens.tx, th.tokens.bg);
    return { fill, ink: i, pressed: pressedOf(fill, i), outline: needsOutline(fill, th.tokens.bg) };
  }
  const sw = th.swatches[(PALETTE_NAMES as readonly string[]).indexOf(spec)];
  return sw ? { fill: sw.fill, ink: sw.ink, pressed: sw.pressed, outline: needsOutline(sw.fill, th.tokens.bg) } : null;
}

export interface WColors {
  fill: number;
  ink: number;
  icon: number;
  cap: number;
  /** 1 px TX3 outline (a fill too close to the ground) */
  outline: boolean;
  /** a 3 px active border colour */
  border?: number;
}
export type TileState = 'on' | 'off' | 'unavailable' | 'value';
export interface WSpec {
  kind: 'button' | 'tile' | 'label';
  role: string;
  color?: string | null;
  on_color?: string | null;
}
const wc = (fill: number, ink: number, icon = ink, cap = ink): WColors => ({ fill, ink, icon, cap, outline: false });
const fromRes = (r: Resolved): WColors => ({ fill: r.fill, ink: r.ink, icon: r.ink, cap: r.ink, outline: r.outline });

/** Mirrors remote/app/ui.c `widget_colors`: the truth for how a widget looks in a theme. */
export function widgetColors(w: WSpec, o: { active?: boolean; state?: TileState; stale?: boolean }, th: RenderTheme): WColors {
  const T = th.tokens;
  if (w.kind === 'tile') {
    let c: WColors;
    const state: TileState = o.state ?? 'value';
    if (state === 'on') {
      const r = resolveColour(w.on_color, th);
      if (r) return fromRes(r);
      c = wc(T.warm, T.onwarm);
    } else if (state === 'unavailable') c = wc(T.s1, T.tx3);
    else if (state === 'off') c = wc(T.s1, T.tx2, T.tx3, T.tx3);
    else c = wc(T.s1, T.tx, T.tx2, T.tx2);
    if (state !== 'on' && state !== 'unavailable' && !o.stale) {
      const r = resolveColour(w.color, th);
      if (r) return fromRes(r);
    }
    if (o.stale && state !== 'on') c.ink = T.tx3;
    return c;
  }
  if (w.kind === 'button') {
    const r = resolveColour(w.color, th);
    if (r) {
      const c = fromRes(r);
      if (o.active) c.border = r.fill === T.acc ? T.tx : T.acc;
      return c;
    }
  }
  if (o.active) return wc(T.acc, T.onacc);
  switch (w.role) {
    case 'primary': return wc(T.s2, T.acc, T.acc, T.tx2);
    case 'secondary': return wc(T.s1, T.tx2, T.tx3, T.tx3);
    case 'danger': return wc(T.s2, T.bad);
    case 'nav': return wc(T.s1, T.tx, T.acc, T.tx2);
    default: return wc(T.s2, T.tx, T.tx2, T.tx2);
  }
}
/** A coloured section label: its colour when readable on the ground, else TX. */
export function labelColor(color: string | null | undefined, th: RenderTheme): number | null {
  const r = resolveColour(color, th);
  return r ? (meets(r.fill, th.tokens.bg, 9, 2) ? r.fill : th.tokens.tx) : null;
}

/** Bytes the colours of one widget add to its page: 0 none, 1 palette only, 3 one free colour, 5 two (page-format v2, section 7). */
export function colourBytes(w: { color?: unknown; on_color?: unknown }): number {
  const c = typeof w.color === 'string' && w.color ? w.color : null;
  const o = typeof w.on_color === 'string' && w.on_color ? w.on_color : null;
  if (!c && !o) return 0;
  return 1 + (isFree(c) ? 2 : 0) + (isFree(o) ? 2 : 0);
}

// ------------------------------------------------------------------ notices
/** One plain sentence for what the linked remote's firmware cannot show (the compiler's notices); null when nothing is lost. */
export function degradeText(msgs: readonly string[] | undefined): string | null {
  if (!msgs?.length) return null;
  return msgs.some((m) => /no colour support|colours dropped/.test(m))
    ? "This remote's firmware has no colour support; widgets keep their role colours until it is updated."
    : "This remote's firmware shows palette colours only; custom colours will use the nearest palette colour until it is updated.";
}
