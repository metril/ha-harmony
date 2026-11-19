// Theme editing, the colour picker's contrast logic and theme files: loaded lazily with the Themes view and the colour field.
import type { Config } from './model';
import { PALETTE_NAMES, TOKEN_NAMES, type Theme, type TokenName } from './themes';
import { contrastX100, meets, type Problem } from './theme-math';
import { nearestPalette, suggestFill, validateTheme } from './theme-fix';
import {
  PRESETS, PRESET_KEYS, activeName, hexOf, isFree, resolveColour, rgbOf, themeNamed, userThemeNames, userThemes, HEX,
  type RenderTheme,
} from './theme-model';

export interface ContrastInfo {
  ratio_x100: number;
  ok: boolean;
  ink: number;
  /** `#RRGGBB` of the nearest fill whose text passes (free colours only) */
  fix?: string;
}
/** "label contrast 5.2:1": the colour against its own best text colour, as the remote chooses it. */
export function contrastInfo(spec: string, th: RenderTheme): ContrastInfo | null {
  const r = resolveColour(spec, th);
  if (!r) return null;
  const ok = meets(r.ink, r.fill, 9, 2);
  return { ratio_x100: contrastX100(r.ink, r.fill), ok, ink: r.ink, ...(ok || !isFree(spec) ? {} : { fix: hexOf(suggestFill(r.fill, th.tokens.tx, th.tokens.bg)) }) };
}
export const ratioText = (x100: number): string => (x100 / 100).toFixed(1);

/** Palette slot (0-based) nearest to a colour in a theme. */
export const nearestSlot = (hex: string, th: RenderTheme): number => nearestPalette(rgbOf(hex), th.swatches.map((s) => s.fill)) - 1;

// ------------------------------------------------------------------ recent colours (per browser)
const RECENT_KEY = 'hh-recent-colours';
export const RECENT_MAX = 8;
export function pushRecent(list: readonly string[], hex: string): string[] {
  return [hex, ...list.filter((h) => h !== hex)].slice(0, RECENT_MAX);
}
export function loadRecent(): string[] {
  try {
    const v = JSON.parse(localStorage.getItem(RECENT_KEY) ?? '[]') as unknown;
    return Array.isArray(v) ? v.filter((h): h is string => typeof h === 'string' && HEX.test(h)).slice(0, RECENT_MAX) : [];
  } catch {
    return [];
  }
}
export function saveRecent(list: readonly string[]): void {
  try {
    localStorage.setItem(RECENT_KEY, JSON.stringify(list));
  } catch { /* storage unavailable */ }
}

// ------------------------------------------------------------------ theme edits
type Rec = Record<string, unknown>;
const cl = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;

export const slugOf = (s: string): string => s.toLowerCase().replace(/[^a-z0-9_]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 31) || 'theme';
/** A free theme name: `base`, then `base_2`, `base_3`... (names are a-z 0-9 _ only). */
export function uniqueThemeName(cfg: Config, base: string): string {
  const used = new Set([...PRESET_KEYS, ...userThemeNames(cfg)]);
  const b = slugOf(base);
  if (!used.has(b)) return b;
  for (let n = 2; ; n++) {
    const s = `${b.slice(0, 31 - String(n).length - 1)}_${n}`;
    if (!used.has(s)) return s;
  }
}
function withThemes(cfg: Config, mut: (themes: Record<string, Rec>) => void): Config {
  const next = cl(cfg);
  const themes = ((next['themes'] as Record<string, Rec> | undefined) ?? {}) as Record<string, Rec>;
  mut(themes);
  if (Object.keys(themes).length) next['themes'] = themes;
  else delete next['themes'];
  return next;
}
/** `theme:` is left out for the default, so a config that never uses themes serialises as before. */
export function selectTheme(cfg: Config, name: string): Config {
  const next = cl(cfg);
  if (name === 'default') delete next['theme'];
  else next['theme'] = name;
  return next;
}
export function createTheme(cfg: Config, name: string, extendsName: string, patch: { tokens?: Record<string, string>; palette?: Record<string, { fill: string }> } = {}): Config {
  return withThemes(cfg, (t) => {
    const o: Rec = { extends: extendsName };
    if (patch.tokens && Object.keys(patch.tokens).length) o['tokens'] = patch.tokens;
    if (patch.palette && Object.keys(patch.palette).length) o['palette'] = patch.palette;
    t[name] = o;
  });
}
/** A copy that extends a preset, or copies a user theme's own overrides (so it does not follow the source). */
export function duplicateTheme(cfg: Config, source: string): { config: Config; name: string } {
  const name = uniqueThemeName(cfg, source in PRESETS ? `my_${source}` : `${source}_copy`);
  const src = userThemes(cfg)[source];
  if (!src) return { config: createTheme(cfg, name, source), name };
  return { config: createTheme(cfg, name, src.extends ?? 'default', { tokens: { ...(src.tokens ?? {}) }, palette: cl(src.palette ?? {}) }), name };
}
export function themeUsage(cfg: Config, name: string): { active: boolean; children: string[] } {
  const themes = userThemes(cfg);
  return { active: activeName(cfg) === name, children: Object.keys(themes).filter((k) => k !== name && themes[k]?.extends === name) };
}
/** Delete a user theme; when it was the active one the config switches to its parent. Children must be re-pointed first (see themeUsage). */
export function deleteTheme(cfg: Config, name: string): Config {
  const parent = userThemes(cfg)[name]?.extends ?? 'default';
  let next = withThemes(cfg, (t) => {
    delete t[name];
  });
  if (activeName(cfg) === name) next = selectTheme(next, parent);
  return next;
}
/** Rename a user theme and every reference to it. */
export function renameTheme(cfg: Config, from: string, to: string): Config {
  let next = withThemes(cfg, (t) => {
    const keys = Object.keys(t);
    const moved: Record<string, Rec> = {};
    for (const k of keys) {
      const v = t[k] as Rec;
      if (v['extends'] === from) v['extends'] = to;
      moved[k === from ? to : k] = v;
    }
    for (const k of keys) delete t[k];
    Object.assign(t, moved);
  });
  if (activeName(cfg) === from) next = selectTheme(next, to);
  return next;
}
function parentOf(cfg: Config, name: string): RenderTheme {
  return themeNamed(cfg, userThemes(cfg)[name]?.extends ?? 'default').theme;
}
/** Override one token of a user theme (an override equal to the parent's value is dropped). */
export function setToken(cfg: Config, name: string, token: TokenName, hex: string): Config {
  const same = hexOf(parentOf(cfg, name).tokens[token]) === hex.toUpperCase();
  return withThemes(cfg, (t) => {
    const th = t[name];
    if (!th) return;
    const tokens = { ...((th['tokens'] as Record<string, string> | undefined) ?? {}) };
    for (const k of Object.keys(tokens)) if (k.toLowerCase() === token) delete tokens[k];
    if (!same) tokens[token] = hex.toUpperCase();
    if (Object.keys(tokens).length) th['tokens'] = tokens;
    else delete th['tokens'];
  });
}
export const resetToken = (cfg: Config, name: string, token: TokenName): Config => withThemes(cfg, (t) => {
  const th = t[name];
  const tokens = { ...((th?.['tokens'] as Record<string, string> | undefined) ?? {}) };
  for (const k of Object.keys(tokens)) if (k.toLowerCase() === token) delete tokens[k];
  if (th) {
    if (Object.keys(tokens).length) th['tokens'] = tokens;
    else delete th['tokens'];
  }
});
/** Override palette slot `slot` (0..11) of a user theme. */
export function setPaletteFill(cfg: Config, name: string, slot: number, hex: string): Config {
  const same = hexOf(parentOf(cfg, name).swatches[slot]?.fill ?? -1) === hex.toUpperCase();
  return withThemes(cfg, (t) => {
    const th = t[name];
    if (!th) return;
    const pal = { ...((th['palette'] as Record<string, { fill: string }> | undefined) ?? {}) };
    if (same) delete pal[String(slot + 1)];
    else pal[String(slot + 1)] = { fill: hex.toUpperCase() };
    if (Object.keys(pal).length) th['palette'] = pal;
    else delete th['palette'];
  });
}
export const resetPaletteFill = (cfg: Config, name: string, slot: number): Config => withThemes(cfg, (t) => {
  const th = t[name];
  const pal = { ...((th?.['palette'] as Record<string, unknown> | undefined) ?? {}) };
  delete pal[String(slot + 1)];
  if (th) {
    if (Object.keys(pal).length) th['palette'] = pal;
    else delete th['palette'];
  }
});
export const overridesToken = (cfg: Config, name: string, token: TokenName): boolean => Object.keys(userThemes(cfg)[name]?.tokens ?? {}).some((k) => k.toLowerCase() === token);
export const overridesSlot = (cfg: Config, name: string, slot: number): boolean => String(slot + 1) in (userThemes(cfg)[name]?.palette ?? {});

// ------------------------------------------------------------------ problems
export interface ThemeProblem extends Problem {
  /** "tx on bg" / "ink on red" in words */
  title: string;
  target: { kind: 'token'; token: TokenName } | { kind: 'slot'; slot: number };
}
export function themeProblems(th: RenderTheme): ThemeProblem[] {
  return validateTheme(th.tokens, th.swatches.map((s) => s.fill)).map((p) => {
    const slot = (PALETTE_NAMES as readonly string[]).indexOf(p.surface);
    return p.text === 'ink'
      ? { ...p, title: `Text on ${p.surface}`, target: { kind: 'slot' as const, slot } }
      : { ...p, title: `${TOKEN_LABEL[p.text as TokenName]?.name ?? p.text} on ${(TOKEN_LABEL[p.surface as TokenName]?.name ?? p.surface).toLowerCase()}`, target: { kind: 'token' as const, token: p.text as TokenName } };
  });
}
export function applyFix(cfg: Config, name: string, p: ThemeProblem): Config {
  return p.target.kind === 'token' ? setToken(cfg, name, p.target.token, hexOf(p.fix)) : setPaletteFill(cfg, name, p.target.slot, hexOf(p.fix));
}
/** Fix every problem, re-checking after each (one fix can settle another). */
export function fixAll(cfg: Config, name: string): Config {
  let next = cfg;
  for (let i = 0; i < 40; i++) {
    const p = themeProblems(themeNamed(next, name).theme)[0];
    if (!p) break;
    next = applyFix(next, name, p);
  }
  return next;
}

// ------------------------------------------------------------------ labels
export const TOKEN_LABEL: Record<TokenName, { name: string; where: string }> = {
  bg: { name: 'Screen', where: 'Behind everything on the remote screen.' },
  s1: { name: 'Card', where: 'Tiles, secondary and page-jump buttons.' },
  s2: { name: 'Button', where: 'Normal buttons, the top bar line.' },
  s3: { name: 'Raised', where: 'Pressed edges and dividers.' },
  tx: { name: 'Main text', where: 'Titles and button labels.' },
  tx2: { name: 'Soft text', where: 'Captions, off tiles, small labels.' },
  tx3: { name: 'Faint text', where: 'Hints, unavailable tiles, outlines.' },
  acc: { name: 'Accent', where: 'Primary text, the running activity, page jumps.' },
  onacc: { name: 'Text on accent', where: 'Labels on the accent fill.' },
  warm: { name: 'On colour', where: 'Tiles that are on.' },
  onwarm: { name: 'Text on warm', where: 'Values on an on tile.' },
  bad: { name: 'Alert', where: 'Power off and danger buttons.' },
  onbad: { name: 'Text on alert', where: 'Labels on the alert fill.' },
};
export const TOKEN_GROUPS: { title: string; tokens: TokenName[] }[] = [
  { title: 'Surfaces', tokens: ['bg', 's1', 's2', 's3'] },
  { title: 'Text', tokens: ['tx', 'tx2', 'tx3'] },
  { title: 'Accent', tokens: ['acc', 'onacc'] },
  { title: 'Warm', tokens: ['warm', 'onwarm'] },
  { title: 'Alert', tokens: ['bad', 'onbad'] },
];

// ------------------------------------------------------------------ import / export
const FORMAT = 'harmony-theme';
/** A self-contained theme file: what it overrides on its base preset (parents flattened). */
export function exportTheme(cfg: Config, name: string): string {
  const th = themeNamed(cfg, name).theme;
  const base = PRESETS[th.base] ?? (PRESETS['default'] as Theme);
  const tokens: Record<string, string> = {};
  for (const k of TOKEN_NAMES) if (th.tokens[k] !== base.tokens[k]) tokens[k] = hexOf(th.tokens[k]);
  const palette: Record<string, { fill: string }> = {};
  th.swatches.forEach((s, i) => {
    if (s.fill !== base.palette[i]?.fill) palette[String(i + 1)] = { fill: hexOf(s.fill) };
  });
  const o: Rec = { format: FORMAT, version: 1, name, extends: th.base };
  if (Object.keys(tokens).length) o['tokens'] = tokens;
  if (Object.keys(palette).length) o['palette'] = palette;
  return JSON.stringify(o, null, 2) + '\n';
}
/** Add a theme file to the config (a taken name becomes name_2). Throws a plain-language Error for a bad file. */
export function importTheme(cfg: Config, text: string): { config: Config; name: string } {
  let o: Rec;
  try {
    o = JSON.parse(text) as Rec;
  } catch {
    throw new Error('This is not a theme file (it is not valid JSON).');
  }
  if (!o || typeof o !== 'object' || o['format'] !== FORMAT) throw new Error('This is not a Harmony theme file.');
  const ext = typeof o['extends'] === 'string' ? o['extends'] : 'default';
  if (!(ext in PRESETS)) throw new Error(`The theme extends "${ext}", which is not a preset (${PRESET_KEYS.join(', ')}).`);
  const name = uniqueThemeName(cfg, typeof o['name'] === 'string' ? o['name'] : 'imported');
  const tokens = Object.fromEntries(Object.entries((o['tokens'] ?? {}) as Record<string, string>).map(([k, v]) => [k.toLowerCase(), v]));
  const palette = (o['palette'] ?? {}) as Record<string, { fill: string }>;
  const config = createTheme(cfg, name, ext, { tokens: Object.fromEntries(Object.entries(tokens).map(([k, v]) => [k, String(v).toUpperCase()])), palette });
  const err = themeNamed(config, name).error;
  if (err) throw new Error(`The theme file is not valid: ${err}`);
  return { config, name };
}
