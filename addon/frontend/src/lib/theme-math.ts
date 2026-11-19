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

export const PAIRS: readonly (readonly [TokenName, TokenName, number, number])[] = [
  ['tx', 'bg', 9, 2],
  ['tx', 's1', 9, 2],
  ['tx', 's2', 9, 2],
  ['tx', 's3', 9, 2],
  ['onacc', 'acc', 9, 2],
  ['onwarm', 'warm', 9, 2],
  ['onbad', 'bad', 9, 2],
  ['tx2', 'bg', 9, 2],
  ['tx2', 's1', 9, 2],
  ['tx3', 'bg', 3, 1],
  ['tx3', 's1', 3, 1],
];
export const WIRE_VERSION = 1;
export const WIRE_LEN = 1 + 2 * TOKEN_NAMES.length + 6 * PALETTE_NAMES.length; // 99
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

/** Wire theme: version, 13 tokens, 12 x (fill, ink, pressed); RGB565 big-endian; fully derived. */
export function serialiseTheme(tokens: Tokens, fills: readonly number[]): Uint8Array {
  if (fills.length !== PALETTE_NAMES.length) throw new Error('need 12 palette fills');
  const t = Object.fromEntries(TOKEN_NAMES.map((k) => [k, snap565(tokens[k])])) as Tokens;
  const out: number[] = [WIRE_VERSION];
  const push = (c: number): void => {
    const v = to565(c);
    out.push(v >> 8, v & 255);
  };
  for (const k of TOKEN_NAMES) push(t[k]);
  for (const f0 of fills) {
    const f = snap565(f0);
    const i = ink(f, t.tx, t.bg);
    push(f);
    push(i);
    push(pressed(f, i));
  }
  return Uint8Array.from(out);
}

type Oklab = [number, number, number];
function toOklab(c: number): Oklab {
  const f = (v: number): number => {
    const x = v / 255;
    return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4;
  };
  const r = f((c >> 16) & 255);
  const g = f((c >> 8) & 255);
  const b = f(c & 255);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}
function fromOklab(L: number, a: number, b: number): [number, boolean] {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  const lin = [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ];
  let out = 0;
  const inside = lin.every((x) => x >= -0.0005 && x <= 1.0005);
  for (let x of lin) {
    x = Math.min(1, Math.max(0, x));
    const v = x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055;
    out = (out << 8) | Math.floor(v * 255 + 0.5);
  }
  return [out, inside];
}

/** Closest RGB565 colour (OKLab lightness stepped by 1/200, hue kept with chroma shrunk to fit the gamut, darker first) for which ok() holds. */
export function nearestPassing(color: number, ok: (c: number) => boolean): number | null {
  const [L, a, b] = toOklab(color);
  for (let k = 1; k <= 200; k++) {
    for (const sign of [-1, 1]) {
      const nl = L + (sign * k) / 200;
      if (nl >= 0 && nl <= 1) {
        let rgb = 0;
        for (const k2 of [1, 0.8, 0.6, 0.4, 0.2, 0]) {
          const [v, inside] = fromOklab(nl, a * k2, b * k2);
          rgb = v;
          if (inside) break;
        }
        const c = snap565(rgb);
        if (ok(c)) return c;
      }
    }
  }
  return null;
}
export function suggestText(text: number, surface: number, num: number, den: number): number {
  const c = nearestPassing(text, (x) => meets(x, surface, num, den));
  if (c !== null) return c;
  return meets(0xf8fcf8, surface, num, den) ? 0xf8fcf8 : 0;
}
export function suggestFill(fill: number, tx: number, bg: number): number {
  const c = nearestPassing(fill, (x) => meets(ink(x, tx, bg), x, 9, 2));
  return c ?? fill;
}

/** Contrast problems of a custom token set (+ optional palette fills against their auto ink); warnings with a nearest-passing fix. */
export function validateTheme(tokens: Tokens, fills?: readonly number[] | null): Problem[] {
  const out: Problem[] = [];
  for (const [a, s, num, den] of PAIRS) {
    if (!meets(tokens[a], tokens[s], num, den)) {
      out.push({
        text: a,
        surface: s,
        ratio_x100: contrastX100(tokens[a], tokens[s]),
        min_x100: Math.floor((100 * num) / den),
        fix: suggestText(tokens[a], tokens[s], num, den),
      });
    }
  }
  (fills ?? []).slice(0, PALETTE_NAMES.length).forEach((f, n) => {
    const i = ink(f, tokens.tx, tokens.bg);
    if (!meets(i, f, 9, 2)) {
      out.push({ text: 'ink', surface: PALETTE_NAMES[n] ?? '', ratio_x100: contrastX100(i, f), min_x100: 450, fix: suggestFill(f, tokens.tx, tokens.bg) });
    }
  });
  return out;
}

/** Wire palette index 1..12 of the fill closest to rgb in OKLab (ties to the lower index). */
export function nearestPalette(rgb: number, fills: readonly number[]): number {
  const p = toOklab(rgb);
  let best = 1;
  let bestD = Infinity;
  fills.forEach((f, i) => {
    const q = toOklab(f);
    const d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2;
    if (d < bestD) {
      best = i + 1;
      bestD = d;
    }
  });
  return best;
}

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
      if (!(TOKEN_NAMES as readonly string[]).includes(k)) throw new Error(`${n}: unknown token ${JSON.stringify(k)}`);
      tokens[k as TokenName] = hex(v, `${n}.tokens.${k}`);
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
