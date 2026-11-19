// Theme maths: TypeScript twin of harmony_core/theme_math.py (pinned by tests/vectors/themes.json).
// Integer maths decides every colour the remote shows; see that module's header for the definitions.
import { LIN, PALETTE_NAMES, TOKEN_NAMES, type Theme, type TokenName } from './themes';

export type Tokens = Record<TokenName, number>;
export interface Problem {
  text: string;
  surface: string;
  ratio_x100: number;
  min_x100: number;
  fix: number;
}

export const MAX_DEPTH = 4;
const K = 2047500;

const y = (c: number): number => 2126 * (LIN[(c >> 16) & 255] ?? 0) + 7152 * (LIN[(c >> 8) & 255] ?? 0) + 722 * (LIN[c & 255] ?? 0);
function hilo(a: number, b: number): [number, number] {
  const ya = y(a) + K;
  const yb = y(b) + K;
  return ya >= yb ? [ya, yb] : [yb, ya];
}

export function contrastX100(a: number, b: number): number {
  const [hi, lo] = hilo(a, b);
  return Math.floor((100 * hi) / lo);
}
export function meets(a: number, b: number, num: number, den: number): boolean {
  const [hi, lo] = hilo(a, b);
  return den * hi >= num * lo;
}
export function snap565(c: number): number {
  const r = Math.min(31, (((c >> 16) & 255) + 4) >> 3);
  const g = Math.min(63, (((c >> 8) & 255) + 2) >> 2);
  const b = Math.min(31, ((c & 255) + 4) >> 3);
  return (r << 19) | (g << 10) | (b << 3);
}
export const is565 = (c: number): boolean => snap565(c) === c;
export function ink(fill: number, tx: number, bg: number): number {
  const [nt, dt] = hilo(fill, tx);
  const [nb, db] = hilo(fill, bg);
  return nt * db >= nb * dt ? tx : bg;
}
export function pressed(fill: number, inkc: number): number {
  let c = 0;
  for (const s of [16, 8, 0]) c |= ((3 * ((fill >> s) & 255) + ((inkc >> s) & 255) + 2) >> 2) << s;
  return snap565(c);
}
export const needsOutline = (fill: number, bg: number): boolean => !meets(fill, bg, 3, 2);
export function to565(c: number): number {
  const s = snap565(c);
  return ((s >> 19) << 11) | (((s >> 10) & 63) << 5) | ((s >> 3) & 31);
}
export const from565 = (v: number): number => (((v >> 11) & 31) << 19) | (((v >> 5) & 63) << 10) | ((v & 31) << 3);

export interface UserTheme {
  extends?: string;
  tokens?: Record<string, string>;
  palette?: Record<string, { fill: string }>;
}
const HEX = /^#[0-9a-fA-F]{6}$/;
function hex(v: unknown, where: string): number {
  if (typeof v !== 'string' || !HEX.test(v)) throw new Error(`${where}: bad colour ${JSON.stringify(v)} (want #RRGGBB)`);
  return snap565(parseInt(v.slice(1), 16));
}

/** Resolve a theme name to snapped tokens + 12 fills; throws on preset-name reuse, unknown token/extends, circular or too deep chains, bad hex. */
export function resolveTheme(name: string, themes: Record<string, UserTheme>, presets: Record<string, Theme>): { tokens: Tokens; fills: number[] } {
  const chain: string[] = [];
  let cur: string = name;
  while (!(cur in presets) || cur in themes) {
    if (cur in presets) throw new Error(`${cur}: a user theme cannot reuse a preset name`);
    if (!(cur in themes)) throw new Error(`unknown theme ${JSON.stringify(cur)}`);
    if (chain.includes(cur)) throw new Error(`${cur}: circular extends`);
    chain.push(cur);
    if (chain.length > MAX_DEPTH) throw new Error(`${name}: extends chain deeper than ${MAX_DEPTH}`);
    const next = themes[cur]?.extends;
    if (typeof next !== 'string') throw new Error(`${cur}: extends must name a preset or another theme`);
    cur = next;
  }
  const base = presets[cur];
  if (!base) throw new Error(`unknown theme ${JSON.stringify(cur)}`);
  const tokens = { ...base.tokens };
  const fills = base.palette.map((s) => s.fill);
  for (const n of [...chain].reverse()) {
    const t = themes[n];
    if (!t) continue;
    for (const [k, v] of Object.entries(t.tokens ?? {})) {
      const key = k.toLowerCase(); // token names are case-insensitive, like the config model
      if (!(TOKEN_NAMES as readonly string[]).includes(key)) throw new Error(`${n}: unknown token ${JSON.stringify(k)}`);
      tokens[key as TokenName] = hex(v, `${n}.tokens.${k}`);
    }
    for (const [k, v] of Object.entries(t.palette ?? {})) {
      const idx = /^\d+$/.test(k) ? Number(k) : 0;
      if (idx < 1 || idx > PALETTE_NAMES.length) throw new Error(`${n}: palette index ${JSON.stringify(k)} must be 1..${PALETTE_NAMES.length}`);
      if (typeof v !== 'object' || v === null || Object.keys(v).join() !== 'fill') throw new Error(`${n}.palette.${k}: want {fill: '#RRGGBB'}`);
      fills[idx - 1] = hex(v.fill, `${n}.palette.${k}.fill`);
    }
  }
  return { tokens, fills };
}
