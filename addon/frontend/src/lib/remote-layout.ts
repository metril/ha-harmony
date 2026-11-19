// Geometry of the Harmony Elite drawing, in one place.
// Everything is a fraction of the remote's body, measured from a product photo (body 267 x 979 px, aspect 3.67:1):
// x fractions are of the body width, y fractions of the body height. The scale is set so the screen is the
// firmware's exact 240x320 logical area. To move or resize a key, edit its row in LAYOUT. Keys are named by the
// backend's snake_case key names (proto/schema.yaml), checked by test/renderer.test.ts. Which firmware key sits at
// the return key and the home-control block (measured on the device) is the one table PHYSICAL_KEYS.

// body size in viewBox units: the photo's screen is 190 px wide, ours 240, so one photo px = 240/190 units
const S = 240 / 190;
export const BODY = { x: 8, y: 8, w: Math.round(267 * S), h: Math.round(979 * S), rTop: 76, rBottom: 84 };
export const VIEW = { w: BODY.w + 16, h: BODY.h + 16 };
/** the mini remote shows just the body (no margin) so its keys stay as large as the width allows */
export const MINI_VIEW = { x: BODY.x - 3, y: BODY.y - 3, w: BODY.w + 6, h: BODY.h + 6 };
const fx = (f: number): number => Math.round((BODY.x + f * BODY.w) * 2) / 2;
const fy = (f: number): number => Math.round((BODY.y + f * BODY.h) * 2) / 2;
/** the 240x320 display, centred where the photo's screen is */
export const SCREEN = { w: 240, h: 320, x: Math.round(fx(0.498) - 120), y: Math.round(fy(0.2217) - 160) };
/** the black glass face around the screen and the Off key */
export const GLASS = { x: BODY.x + 14, y: BODY.y + 14, w: BODY.w - 28, h: Math.round(0.371 * BODY.h), r: 30 };

/** Body outline: a broad dome at the top, a rounder end at the bottom. */
export function bodyPath(): string {
  const { x, y, w, h } = BODY;
  const t = BODY.rTop, b = BODY.rBottom;
  return `M${x + t} ${y}h${w - 2 * t}a${t} ${t} 0 0 1 ${t} ${t}v${h - t - b}a${b} ${b} 0 0 1 -${b} ${b}h-${w - 2 * b}a${b} ${b} 0 0 1 -${b} -${b}v-${h - t - b}a${t} ${t} 0 0 1 ${t} -${t}z`;
}
/** y of the seam above the matte grip (0.90 of the body height) */
export const GRIP_Y = Math.round(BODY.y + 0.9 * BODY.h);
/** the matte grip below the seam */
export function gripPath(): string {
  const { x, w } = BODY;
  const b = BODY.rBottom;
  const yEnd = BODY.y + BODY.h;
  return `M${x} ${GRIP_Y}h${w}v${yEnd - GRIP_Y - b}a${b} ${b} 0 0 1 -${b} ${b}h-${w - 2 * b}a${b} ${b} 0 0 1 -${b} -${b}z`;
}

/**
 * MAPPING TABLE (measured on the device's input): which firmware key sits at each physical position.
 * All 35 firmware keys must appear exactly once in KEYS.
 */
export const PHYSICAL_KEYS = {
  /** the return/back key beside Mute: evdev 412 PrevChannel (Exit is a separate key, top left) */
  return: 'prev_channel',
  /** home-control keys: two lights stacked on the left, two sockets stacked on the right */
  lightUpper: 'home_control_1',
  lightLower: 'home_control_2',
  socketUpper: 'home_control_3',
  socketLower: 'home_control_4',
} as const;

export type KeyShape = 'rect' | 'pill' | 'circle' | 'rock-top' | 'rock-bottom' | 'path';
type Radii = [number, number, number, number];
export interface KeyGeom {
  name: string;
  /** stock spelling, for tooltips */
  stock: string;
  /** short text drawn on the key ('' = a drawn glyph or nothing) */
  label: string;
  /** what is printed on the real key, when it differs from the stock name (accessible name, tooltip) */
  printed?: string;
  group: string;
  shape: KeyShape;
  /** bounding box */
  x: number;
  y: number;
  w: number;
  h: number;
  /** colour keys: the colour of the ring drawn inside the key */
  color?: string;
  /** rect: corner radii (top-left, top-right, bottom-right, bottom-left) */
  radii?: Radii;
  /** shape 'path': the outline */
  d?: string;
}
type Opts = { printed?: string; color?: string; radii?: Radii };
/** name, label, group, shape, centre x, centre y, width, height (fractions of the body), options */
type Row = readonly [string, string, string, KeyShape, number, number, number, number, Opts?];

/** Static printed text on the body (not a control). `sub` is a second line under a rule (Ch over Pg). */
export interface Printed { text: string; x: number; y: number; sub?: string }

const STOCK: Record<string, string> = { prev_channel: 'PrevChannel', home_control_1: 'HomeControl1', home_control_2: 'HomeControl2', home_control_3: 'HomeControl3', home_control_4: 'HomeControl4' };
const stockOf = (name: string): string => STOCK[name] ?? name.replace(/(^|_)([a-z0-9])/g, (_m, _u, c: string) => c.toUpperCase());
const geom = ([name, label, group, shape, cx, cy, w, h, o]: Row): KeyGeom => ({
  name, stock: stockOf(name), label, group, shape,
  x: fx(cx) - (w * BODY.w) / 2, y: fy(cy) - (h * BODY.h) / 2, w: w * BODY.w, h: h * BODY.h, ...o,
});

// ---- the cluster round the D-pad: Exit, Menu, Vol, Ch/Pg, Mute, Back and a rounded diamond with OK.
// Its outlines are traced in a 3x zoomed crop of the photo (crop px); cp() maps them into viewBox units.
type Pt = [number, number];
const cp = (cx: number, cy: number): Pt => {
  const px = 421 + (cx - 62) / 3, py = 532 + (cy - 35) / 3;
  return [fx((px - 394) / 267), fy((py - 2) / 979)];
};
const mx = (cx: number): number => 760 - cx; // the cluster is symmetric about crop x = 380
const flip = (crop: Pt[]): Pt[] => crop.map(([x, y]) => [mx(x), y] as Pt);
/** a closed polygon with rounded corners (radius r in viewBox units) */
function roundedPoly(pts: Pt[], r: number): string {
  const n = pts.length;
  let d = '';
  pts.forEach((c, i) => {
    const p = pts[(i + n - 1) % n] as Pt, q = pts[(i + 1) % n] as Pt;
    const d1 = Math.hypot(p[0] - c[0], p[1] - c[1]), d2 = Math.hypot(q[0] - c[0], q[1] - c[1]);
    const k = Math.min(r, d1 / 2, d2 / 2);
    const a: Pt = [c[0] + ((p[0] - c[0]) * k) / d1, c[1] + ((p[1] - c[1]) * k) / d1];
    const b: Pt = [c[0] + ((q[0] - c[0]) * k) / d2, c[1] + ((q[1] - c[1]) * k) / d2];
    d += `${i ? 'L' : 'M'}${a[0].toFixed(1)} ${a[1].toFixed(1)}Q${c[0].toFixed(1)} ${c[1].toFixed(1)} ${b[0].toFixed(1)} ${b[1].toFixed(1)}`;
  });
  return d + 'z';
}
const bbox = (pts: Pt[]) => {
  const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
  const x = Math.min(...xs), y = Math.min(...ys);
  return { x, y, w: Math.max(...xs) - x, h: Math.max(...ys) - y };
};
const polyKey = (name: string, group: string, crop: Pt[], o: { label?: string; printed?: string } = {}): KeyGeom => {
  const pts = crop.map(([x, y]) => cp(x, y));
  return { name, stock: stockOf(name), label: o.label ?? '', group, shape: 'path', ...bbox(pts), d: roundedPoly(pts, 5), ...(o.printed ? { printed: o.printed } : {}) };
};
const EXIT: Pt[] = [[62, 35], [345, 35], [345, 60], [290, 113], [62, 113]];
const MUTE: Pt[] = [[62, 503], [290, 503], [345, 558], [345, 580], [62, 580]];
const VOL_TOP: Pt[] = [[62, 152], [215, 152], [152, 308], [62, 308]];
const VOL_BOT: Pt[] = [[62, 308], [152, 308], [215, 465], [62, 465]];

// the diamond (a square turned 45 degrees, rounded), split into four kites that meet round the OK button
// the sharp square's half-diagonal is 239 crop px; rounding the tips (0.45 along each flank) leaves a visible half of ~185,
// which puts the side points a thin gap from Vol and Ch/Pg and the top and bottom points between Exit/Menu and Mute/Back
const DC = cp(382, 308);
const DT = cp(382, 69), DR_ = cp(621, 308), DB = cp(382, 547), DL = cp(143, 308);
const OK_R = Math.round((80 / 3) * (BODY.w / 267));
/** diamond centre and half-diagonal in viewBox units */
export const DPAD = { x: DC[0], y: DC[1], r: Math.round((DR_[0] - DL[0]) / 2) };
const lerp = (a: Pt, b: Pt, t: number): Pt => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
/** one direction of the diamond: the region round tip `tip`, between its neighbours `a` and `b`; `ang` is the tip's bearing from the centre */
const kite = (name: string, tip: Pt, a: Pt, b: Pt, ang: number): KeyGeom => {
  const mid = (p: Pt, q: Pt): Pt => [(p[0] + q[0]) / 2, (p[1] + q[1]) / 2];
  const ra = OK_R + 4;
  const on = (t: number): Pt => [DC[0] + ra * Math.cos(t), DC[1] + ra * Math.sin(t)];
  const a0 = on(ang - Math.PI / 4), a1 = on(ang + Math.PI / 4);
  const m1 = mid(tip, a), m2 = mid(tip, b);
  const p1 = lerp(tip, a, 0.45), p2 = lerp(tip, b, 0.45);
  const f = (p: Pt) => `${p[0].toFixed(1)} ${p[1].toFixed(1)}`;
  const d = `M${f(a0)}L${f(m1)}L${f(p1)}Q${f(tip)} ${f(p2)}L${f(m2)}L${f(a1)}A${ra} ${ra} 0 0 0 ${f(a0)}z`;
  return { name, stock: stockOf(name), label: '', group: 'nav', shape: 'path', ...bbox([m1, p1, tip, p2, m2, a0, a1]), d };
};
const SPECIAL: Record<string, KeyGeom> = {};
[
  polyKey('exit', 'menu', EXIT, { label: 'Exit' }),
  polyKey('menu', 'menu', flip(EXIT), { label: 'Menu' }),
  polyKey('volume_up', 'volume', VOL_TOP, { printed: 'Vol +' }),
  polyKey('volume_down', 'volume', VOL_BOT, { printed: 'Vol −' }),
  polyKey('channel_up', 'channel', flip(VOL_TOP), { printed: 'Ch/Pg +' }),
  polyKey('channel_down', 'channel', flip(VOL_BOT), { printed: 'Ch/Pg −' }),
  polyKey('volume_mute', 'volume', MUTE, { printed: 'Mute' }),
  polyKey(PHYSICAL_KEYS.return, 'channel', flip(MUTE), { printed: 'Back' }),
  kite('up', DT, DL, DR_, -Math.PI / 2),
  kite('left', DL, DB, DT, Math.PI),
  kite('right', DR_, DT, DB, 0),
  kite('down', DB, DR_, DL, Math.PI / 2),
  { name: 'select', stock: 'Select', label: 'OK', group: 'nav', shape: 'circle' as const, x: DC[0] - OK_R, y: DC[1] - OK_R, w: 2 * OK_R, h: 2 * OK_R },
].forEach((g) => { SPECIAL[g.name] = g; });

/** Printed text beside the rocker keys: "Vol", and "Ch" over "Pg" with a rule, at mid height near the outer edge. */
const [vx, vy] = cp(95, 308), [chx, chy] = cp(658, 318);
export const PRINTED: readonly Printed[] = [
  { text: 'Vol', x: vx, y: vy + 3 },
  { text: 'Ch', sub: 'Pg', x: chx, y: chy - 6 },
];

const PK = PHYSICAL_KEYS;
/** In keyboard order: top to bottom, left to right. A string is a pre-built key of the D-pad cluster above. */
const LAYOUT: readonly (Row | string)[] = [
  ['power_off_activity', 'Off', 'power', 'pill', 0.242, 0.047, 0.176, 0.011],
  ['home', 'ACTIVITIES', 'strip', 'rect', 0.257, 0.422, 0.258, 0.022, { printed: 'ACTIVITIES', radii: [0, 0, 0, 0] }],
  ['favorites', 'DEVICES', 'strip', 'rect', 0.740, 0.422, 0.258, 0.022, { printed: 'DEVICES', radii: [0, 0, 0, 0] }],
  ['rewind', '', 'transport', 'rect', 0.232, 0.473, 0.262, 0.026, { radii: [11, 3, 3, 3] }],
  ['play', '', 'transport', 'rock-top', 0.498, 0.477, 0.150, 0.033],
  ['fast_forward', '', 'transport', 'rect', 0.764, 0.473, 0.262, 0.026, { radii: [3, 11, 3, 3] }],
  ['record', '', 'transport', 'rect', 0.232, 0.513, 0.262, 0.026, { radii: [3, 3, 3, 11] }],
  ['pause', '', 'transport', 'rock-bottom', 0.498, 0.510, 0.150, 0.033],
  ['stop', '', 'transport', 'rect', 0.764, 0.513, 0.262, 0.026, { radii: [3, 3, 11, 3] }],
  'exit',
  'menu',
  'volume_up',
  'up',
  'left',
  'select',
  'right',
  'down',
  'channel_up',
  'volume_down',
  'channel_down',
  'volume_mute',
  PK.return,
  ['dvr', 'DVR', 'guide', 'rect', 0.232, 0.759, 0.262, 0.027, { radii: [14, 2, 2, 14] }],
  ['guide', 'Guide', 'guide', 'rect', 0.496, 0.759, 0.266, 0.027, { radii: [2, 2, 2, 2] }],
  ['info', 'Info', 'guide', 'rect', 0.762, 0.759, 0.266, 0.027, { radii: [2, 14, 14, 2] }],
  ['red', 'Red', 'colour', 'rect', 0.1825, 0.800, 0.163, 0.020, { color: '#e0584c', radii: [11, 4, 4, 11] }],
  ['green', 'Green', 'colour', 'rect', 0.3928, 0.800, 0.163, 0.020, { color: '#4fb56a', radii: [4, 4, 4, 4] }],
  ['yellow', 'Yellow', 'colour', 'rect', 0.6031, 0.800, 0.163, 0.020, { color: '#e6c23a', radii: [4, 4, 4, 4] }],
  ['blue', 'Blue', 'colour', 'rect', 0.8134, 0.800, 0.163, 0.020, { color: '#5b8fe0', radii: [4, 11, 11, 4] }],
  [PK.lightUpper, '', 'home', 'rect', 0.234, 0.836, 0.266, 0.026, { printed: 'upper light', radii: [6, 6, 6, 6] }],
  ['home_control_plus', '+', 'home', 'rock-top', 0.498, 0.840, 0.150, 0.033],
  [PK.socketUpper, '', 'home', 'rect', 0.762, 0.836, 0.266, 0.026, { printed: 'upper socket', radii: [6, 6, 6, 6] }],
  [PK.lightLower, '', 'home', 'rect', 0.234, 0.876, 0.266, 0.027, { printed: 'lower light', radii: [6, 6, 6, 20] }],
  ['home_control_minus', '−', 'home', 'rock-bottom', 0.498, 0.873, 0.150, 0.033],
  [PK.socketLower, '', 'home', 'rect', 0.762, 0.876, 0.266, 0.027, { printed: 'lower socket', radii: [6, 6, 20, 6] }],
];
/** Keys that are one visual plate with separate hit areas (no seam drawn at rest): the D-pad, Vol, Ch/Pg and the home +/- key. */
export const JOINED: ReadonlySet<string> = new Set(['up', 'down', 'left', 'right', 'volume_up', 'volume_down', 'channel_up', 'channel_down', 'home_control_plus', 'home_control_minus']);
/** The resting outline of Vol and Ch/Pg: one continuous plate each (their halves are invisible hit areas at rest). */
export const PLATES: readonly string[] = [[[62, 152], [215, 152], [152, 308], [215, 465], [62, 465]] as Pt[], flip([[62, 152], [215, 152], [152, 308], [215, 465], [62, 465]])].map((c) => roundedPoly(c.map(([x, y]) => cp(x, y)), 5));
/** Keys whose halves are invisible at rest and drawn over a PLATE */
export const PLATED: ReadonlySet<string> = new Set(['volume_up', 'volume_down', 'channel_up', 'channel_down']);
export const KEYS: readonly KeyGeom[] = LAYOUT.map((r) => (typeof r === 'string' ? (SPECIAL[r] as KeyGeom) : geom(r)));

export const keyByName = new Map(KEYS.map((g) => [g.name, g]));
/** "Volume Up" style name for tooltips and announcements, plus what is printed on the real key when that differs */
export const keyTitle = (name: string): string => {
  const g = keyByName.get(name);
  if (!g) return name;
  const t = g.stock.replace(/([a-z0-9])([A-Z])/g, '$1 $2');
  return g.printed && g.printed.toLowerCase() !== t.toLowerCase() ? `${t} (${g.printed})` : t;
};

/** SVG path of a key's outline. */
export function keyPath(g: KeyGeom): string {
  const { x, y, w, h } = g;
  const rr4 = (a: number, b: number, c: number, d: number) => `M${x + a} ${y}h${w - a - b}a${b} ${b} 0 0 1 ${b} ${b}v${h - b - c}a${c} ${c} 0 0 1 -${c} ${c}h-${w - c - d}a${d} ${d} 0 0 1 -${d} -${d}v-${h - d - a}a${a} ${a} 0 0 1 ${a} -${a}z`;
  switch (g.shape) {
    case 'path':
      return g.d ?? '';
    case 'circle': {
      const r = w / 2;
      return `M${x} ${y + r}a${r} ${r} 0 1 0 ${w} 0a${r} ${r} 0 1 0 -${w} 0z`;
    }
    case 'pill':
      return rr4(h / 2, h / 2, h / 2, h / 2);
    case 'rect': {
      const [a, b, c, d] = g.radii ?? [6, 6, 6, 6];
      return rr4(a, b, c, d);
    }
    // one half of a joined key: rounded on the outer end, square where the two halves meet
    case 'rock-top':
      return rr4(7, 7, 0, 0);
    case 'rock-bottom':
      return rr4(0, 0, 7, 7);
  }
}
/** The outlined pill drawn inside a colour key, in its colour. */
export function tintPath(g: KeyGeom): string {
  const iw = g.w * 0.6, ih = g.h * 0.45, ix = g.x + (g.w - iw) / 2, iy = g.y + (g.h - ih) / 2, r = ih / 2;
  return `M${ix + r} ${iy}h${iw - 2 * r}a${r} ${r} 0 0 1 0 ${ih}h-${iw - 2 * r}a${r} ${r} 0 0 1 0 -${ih}z`;
}
