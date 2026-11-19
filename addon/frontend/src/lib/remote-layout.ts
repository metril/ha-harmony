// Geometry of the Harmony Elite drawing, in one place.
// Everything is a fraction of the remote's body, measured from a product photo (body 267 x 979 px, aspect 3.67:1):
// x fractions are of the body width, y fractions of the body height. The scale is set so the screen is the
// firmware's exact 240x320 logical area. To move or resize a key, edit its row in LAYOUT. Keys are named by the
// backend's snake_case key names (proto/schema.yaml), checked by test/renderer.test.ts. Which firmware key sits at
// the return key and the home-control block (measured on the device) is the one table PHYSICAL_KEYS.

// body size in viewBox units: the photo's screen is 190 px wide, ours 240, so one photo px = 240/190 units
const S = 240 / 190;
export const BODY = { x: 8, y: 8, w: Math.round(267 * S), h: Math.round(979 * S), rTop: 46, rBottom: 84 };
export const VIEW = { w: BODY.w + 16, h: BODY.h + 16 };
/** the mini remote shows just the body (no margin) so its keys stay as large as the width allows */
export const MINI_VIEW = { x: BODY.x - 3, y: BODY.y - 3, w: BODY.w + 6, h: BODY.h + 6 };
const fx = (f: number): number => Math.round((BODY.x + f * BODY.w) * 2) / 2;
const fy = (f: number): number => Math.round((BODY.y + f * BODY.h) * 2) / 2;
/** the 240x320 display, centred where the photo's screen is */
export const SCREEN = { w: 240, h: 320, x: Math.round(fx(0.498) - 120), y: Math.round(fy(0.2217) - 160) };
/** the black glass face around the screen and the Off key */
export const GLASS = { x: BODY.x + 14, y: BODY.y + 14, w: BODY.w - 28, h: Math.round(0.371 * BODY.h), r: 30 };

/** Rounded-rectangle path of the body: tighter at the top, a rounder grip at the bottom. `inset` shrinks it. */
export function bodyPath(inset = 0): string {
  const x = BODY.x + inset, y = BODY.y + inset, w = BODY.w - 2 * inset, h = BODY.h - 2 * inset;
  const t = BODY.rTop - inset, b = BODY.rBottom - inset;
  return `M${x + t} ${y}h${w - 2 * t}a${t} ${t} 0 0 1 ${t} ${t}v${h - t - b}a${b} ${b} 0 0 1 -${b} ${b}h-${w - 2 * b}a${b} ${b} 0 0 1 -${b} -${b}v-${h - t - b}a${t} ${t} 0 0 1 ${t} -${t}z`;
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

export type KeyShape = 'rect' | 'pill' | 'circle' | 'rock-top' | 'rock-bottom' | 'wedge-up' | 'wedge-down' | 'wedge-left' | 'wedge-right';
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
  x: number;
  y: number;
  w: number;
  h: number;
  /** colour keys */
  color?: string;
}
type Opts = { printed?: string; color?: string };
/** name, label, group, shape, centre x, centre y, width, height (fractions of the body), options */
type Row = readonly [string, string, string, KeyShape, number, number, number, number, Opts?];

/** Static printed text on the body (not a control): text, centre x, y (fractions). */
export const PRINTED: readonly { text: string; x: number; y: number }[] = [
  { text: 'Vol', x: fx(0.170), y: fy(0.634) + 3 },
  { text: 'Ch/Pg', x: fx(0.825), y: fy(0.634) + 3 },
];

const STOCK: Record<string, string> = { prev_channel: 'PrevChannel', home_control_1: 'HomeControl1', home_control_2: 'HomeControl2', home_control_3: 'HomeControl3', home_control_4: 'HomeControl4' };
const stockOf = (name: string): string => STOCK[name] ?? name.replace(/(^|_)([a-z0-9])/g, (_m, _u, c: string) => c.toUpperCase());
const geom = ([name, label, group, shape, cx, cy, w, h, o]: Row): KeyGeom => ({
  name, stock: stockOf(name), label, group, shape,
  x: fx(cx) - (w * BODY.w) / 2, y: fy(cy) - (h * BODY.h) / 2, w: w * BODY.w, h: h * BODY.h, ...o,
});

// the d-pad: a ring around OK. Centre and radii as fractions of the body width.
const DPAD_C = { x: 0.498, y: 0.633 };
const DPAD_R = 0.24;
const DPAD_IN = 0.127;
/** ring centre and outer radius in viewBox units (the ring is drawn behind the wedges) */
export const DPAD = { x: fx(DPAD_C.x), y: fy(DPAD_C.y), r: Math.round(DPAD_R * BODY.w) };
const DR = DPAD.r;
const DI = Math.round(DPAD_IN * BODY.w);
const dpadKeys: KeyGeom[] = ([
  { name: 'up', label: '', group: 'nav', shape: 'wedge-up', x: DPAD.x - DR + 14, y: DPAD.y - DR, w: 2 * DR - 28, h: DR - DI },
  { name: 'left', label: '', group: 'nav', shape: 'wedge-left', x: DPAD.x - DR, y: DPAD.y - DR + 14, w: DR - DI, h: 2 * DR - 28 },
  { name: 'select', label: 'OK', group: 'nav', shape: 'circle', x: DPAD.x - DI, y: DPAD.y - DI, w: 2 * DI, h: 2 * DI },
  { name: 'right', label: '', group: 'nav', shape: 'wedge-right', x: DPAD.x + DI, y: DPAD.y - DR + 14, w: DR - DI, h: 2 * DR - 28 },
  { name: 'down', label: '', group: 'nav', shape: 'wedge-down', x: DPAD.x - DR + 14, y: DPAD.y + DI, w: 2 * DR - 28, h: DR - DI },
] satisfies Omit<KeyGeom, 'stock'>[]).map((g) => ({ ...g, stock: stockOf(g.name) }));

const PK = PHYSICAL_KEYS;
/** In keyboard order: top to bottom, left to right. */
const LAYOUT: readonly (Row | 'dpad')[] = [
  ['power_off_activity', 'Off', 'power', 'pill', 0.243, 0.048, 0.165, 0.014, { color: '#7a2b27' }],
  ['home', 'ACTIVITIES', 'strip', 'rect', 0.257, 0.423, 0.258, 0.022, { printed: 'ACTIVITIES' }],
  ['favorites', 'DEVICES', 'strip', 'rect', 0.740, 0.423, 0.258, 0.022, { printed: 'DEVICES' }],
  ['rewind', '', 'transport', 'rect', 0.232, 0.474, 0.262, 0.026],
  ['play', '', 'transport', 'rock-top', 0.494, 0.478, 0.142, 0.032],
  ['fast_forward', '', 'transport', 'rect', 0.764, 0.474, 0.262, 0.026],
  ['record', '', 'transport', 'rect', 0.232, 0.513, 0.262, 0.026],
  ['pause', '', 'transport', 'rock-bottom', 0.494, 0.511, 0.142, 0.030],
  ['stop', '', 'transport', 'rect', 0.764, 0.513, 0.262, 0.026],
  ['exit', 'Exit', 'menu', 'rect', 0.311, 0.555, 0.419, 0.027],
  ['menu', 'Menu', 'menu', 'rect', 0.723, 0.555, 0.345, 0.027],
  ['volume_up', '+', 'volume', 'rock-top', 0.170, 0.604, 0.139, 0.043, { printed: 'Vol +' }],
  'dpad',
  ['channel_up', '+', 'channel', 'rock-top', 0.825, 0.604, 0.139, 0.043, { printed: 'Ch/Pg +' }],
  ['volume_down', '−', 'volume', 'rock-bottom', 0.170, 0.667, 0.139, 0.043, { printed: 'Vol −' }],
  ['channel_down', '−', 'channel', 'rock-bottom', 0.825, 0.667, 0.139, 0.043, { printed: 'Ch/Pg −' }],
  ['volume_mute', 'Mute', 'volume', 'rect', 0.275, 0.713, 0.348, 0.026],
  [PK.return, '', 'channel', 'rect', 0.723, 0.713, 0.345, 0.026, { printed: 'Back' }],
  ['dvr', 'DVR', 'guide', 'rect', 0.228, 0.759, 0.255, 0.026],
  ['guide', 'Guide', 'guide', 'rect', 0.498, 0.759, 0.240, 0.026],
  ['info', 'Info', 'guide', 'rect', 0.768, 0.759, 0.255, 0.026],
  ['red', 'Red', 'colour', 'pill', 0.191, 0.797, 0.180, 0.016, { color: '#b8423a' }],
  ['green', 'Green', 'colour', 'pill', 0.401, 0.797, 0.180, 0.016, { color: '#3e8e52' }],
  ['yellow', 'Yellow', 'colour', 'pill', 0.596, 0.797, 0.180, 0.016, { color: '#bc9727' }],
  ['blue', 'Blue', 'colour', 'pill', 0.805, 0.797, 0.180, 0.016, { color: '#3b69b8' }],
  [PK.lightUpper, '', 'home', 'rect', 0.232, 0.837, 0.262, 0.030, { printed: 'upper light' }],
  ['home_control_plus', '+', 'home', 'rock-top', 0.494, 0.838, 0.142, 0.032],
  [PK.socketUpper, '', 'home', 'rect', 0.764, 0.837, 0.262, 0.030, { printed: 'upper socket' }],
  [PK.lightLower, '', 'home', 'rect', 0.232, 0.876, 0.262, 0.030, { printed: 'lower light' }],
  ['home_control_minus', '−', 'home', 'rock-bottom', 0.494, 0.874, 0.142, 0.033],
  [PK.socketLower, '', 'home', 'rect', 0.764, 0.876, 0.262, 0.030, { printed: 'lower socket' }],
];
export const KEYS: readonly KeyGeom[] = LAYOUT.flatMap((r) => (r === 'dpad' ? dpadKeys : [geom(r)]));

export const keyByName = new Map(KEYS.map((g) => [g.name, g]));
/** "Volume Up" style name for tooltips and announcements, plus what is printed on the real key when that differs */
export const keyTitle = (name: string): string => {
  const g = keyByName.get(name);
  if (!g) return name;
  const t = g.stock.replace(/([a-z0-9])([A-Z])/g, '$1 $2');
  return g.printed && g.printed.toLowerCase() !== t.toLowerCase() ? `${t} (${g.printed})` : t;
};

/** SVG path for a key shape in its own box (x,y,w,h). */
export function keyPath(g: KeyGeom): string {
  const { x, y, w, h } = g;
  const rr = (r: number) => `M${x + r} ${y}h${w - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}v${h - 2 * r}a${r} ${r} 0 0 1 -${r} ${r}h-${w - 2 * r}a${r} ${r} 0 0 1 -${r} -${r}v-${h - 2 * r}a${r} ${r} 0 0 1 ${r} -${r}z`;
  // per-corner radii: top-left, top-right, bottom-right, bottom-left
  const rr4 = (a: number, b: number, c: number, d: number) => `M${x + a} ${y}h${w - a - b}a${b} ${b} 0 0 1 ${b} ${b}v${h - b - c}a${c} ${c} 0 0 1 -${c} ${c}h-${w - c - d}a${d} ${d} 0 0 1 -${d} -${d}v-${h - d - a}a${a} ${a} 0 0 1 ${a} -${a}z`;
  switch (g.shape) {
    case 'circle': {
      const r = w / 2;
      return `M${x} ${y + r}a${r} ${r} 0 1 0 ${w} 0a${r} ${r} 0 1 0 -${w} 0z`;
    }
    case 'pill':
      return rr(h / 2);
    case 'rect':
      return rr(8);
    // one half of a rocker: rounded on the outer end, nearly square where the two halves meet
    case 'rock-top':
      return rr4(10, 10, 3, 3);
    case 'rock-bottom':
      return rr4(3, 3, 10, 10);
    // d-pad wedges: a rounded outer edge, a straight inner edge towards the OK button
    case 'wedge-up':
      return `M${x + 4} ${y + h}L${x + 12} ${y + 8}Q${x + w / 2} ${y - 4} ${x + w - 12} ${y + 8}L${x + w - 4} ${y + h}Q${x + w / 2} ${y + h + 6} ${x + 4} ${y + h}z`;
    case 'wedge-down':
      return `M${x + 4} ${y}Q${x + w / 2} ${y - 6} ${x + w - 4} ${y}L${x + w - 12} ${y + h - 8}Q${x + w / 2} ${y + h + 4} ${x + 12} ${y + h - 8}z`;
    case 'wedge-left':
      return `M${x + w} ${y + 4}Q${x + w + 6} ${y + h / 2} ${x + w} ${y + h - 4}L${x + 8} ${y + h - 12}Q${x - 4} ${y + h / 2} ${x + 8} ${y + 12}z`;
    case 'wedge-right':
      return `M${x} ${y + 4}L${x + w - 8} ${y + 12}Q${x + w + 4} ${y + h / 2} ${x + w - 8} ${y + h - 12}L${x} ${y + h - 4}Q${x - 6} ${y + h / 2} ${x} ${y + 4}z`;
  }
}
