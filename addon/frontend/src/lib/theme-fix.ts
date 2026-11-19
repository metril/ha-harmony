// Theme maths for the editor only (contrast fixes in OKLab, the validation pairs, the wire theme): lazy, never in the entry chunk.
import { PALETTE_NAMES, TOKEN_NAMES, type TokenName } from './themes';
import { contrastX100, ink, meets, pressed, snap565, to565, type Problem, type Tokens } from './theme-math';

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