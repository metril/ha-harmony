// Geometry of the Harmony Elite drawing, in one place, in a 340x1160 viewBox.
// Positions are traced from a product photo (written below in photo pixels: `r(x0, y0, x1, y1)`) and scaled so the
// screen is the firmware's exact 240x320 logical area. Keys are keyed by the backend's snake_case key names
// (proto/schema.yaml), checked by test/renderer.test.ts. Which firmware key sits at the return key and the
// home-control block (measured on the device) is the one table PHYSICAL_KEYS.
export const VIEW = { w: 340, h: 1160 };

// photo pixels -> viewBox units (the photo's screen is 190 px wide, ours 240)
const S = 240 / 190;
const X = (p: number): number => Math.round(170 + (p - 527) * S);
const Y = (p: number): number => Math.round(86 + (p - 92) * S);
const r = (x0: number, y0: number, x1: number, y1: number) => ({ x: X(x0), y: Y(y0), w: X(x1) - X(x0), h: Y(y1) - Y(y0) });

export const BODY = { x: 8, y: 8, w: 324, h: 1143, r: 52 };
export const GLASS = { x: 20, y: 16, w: 300, h: 436, r: 30 };
/** the 240x320 display */
export const SCREEN = { x: 50, y: 86, w: 240, h: 320 };

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
/** Static printed text on the body (not a control). */
export const PRINTED: readonly { text: string; x: number; y: number }[] = [
  { text: 'Vol', x: X(439), y: Y(624) + 3 },
  { text: 'Ch/Pg', x: X(615), y: Y(624) + 3 },
];

const STOCK: Record<string, string> = { prev_channel: 'PrevChannel', home_control_1: 'HomeControl1', home_control_2: 'HomeControl2', home_control_3: 'HomeControl3', home_control_4: 'HomeControl4' };
const stockOf = (name: string): string => STOCK[name] ?? name.replace(/(^|_)([a-z0-9])/g, (_m, _u, c: string) => c.toUpperCase());
type Box = { x: number; y: number; w: number; h: number };
const k = (name: string, label: string, group: string, shape: KeyShape, b: Box, o: { printed?: string; color?: string } = {}): KeyGeom => ({ name, stock: stockOf(name), label, group, shape, ...b, ...o });

// the d-pad: a ring around OK, centred at photo (527, 622)
const DC = { x: X(527), y: Y(622) };
const DR = Math.round(62 * S);
const DI = Math.round(34 * S);
export const DPAD = { x: DC.x, y: DC.y, r: DR };
const PK = PHYSICAL_KEYS;

/** In keyboard order: top to bottom, left to right. */
export const KEYS: readonly KeyGeom[] = [
  k('power_off_activity', 'Off', 'power', 'pill', r(437, 42, 481, 56), { color: '#7a2b27' }),
  k('home', 'ACTIVITIES', 'strip', 'rect', r(428, 405, 497, 427), { printed: 'ACTIVITIES' }),
  k('favorites', 'DEVICES', 'strip', 'rect', r(557, 405, 626, 427), { printed: 'DEVICES' }),
  k('rewind', '', 'transport', 'rect', r(421, 454, 491, 479)),
  k('play', '', 'transport', 'rock-top', r(507, 454, 545, 485)),
  k('fast_forward', '', 'transport', 'rect', r(563, 454, 633, 479)),
  k('record', '', 'transport', 'rect', r(421, 492, 491, 517)),
  k('pause', '', 'transport', 'rock-bottom', r(507, 488, 545, 517)),
  k('stop', '', 'transport', 'rect', r(563, 492, 633, 517)),
  k('exit', 'Exit', 'menu', 'rect', r(421, 532, 533, 558)),
  k('menu', 'Menu', 'menu', 'rect', r(541, 532, 633, 558)),
  k('volume_up', '+', 'volume', 'rock-top', r(421, 572, 458, 622), { printed: 'Vol +' }),
  k('up', '', 'nav', 'wedge-up', { x: DC.x - DR + 14, y: DC.y - DR, w: 2 * DR - 28, h: DR - DI }),
  k('left', '', 'nav', 'wedge-left', { x: DC.x - DR, y: DC.y - DR + 14, w: DR - DI, h: 2 * DR - 28 }),
  k('select', 'OK', 'nav', 'circle', { x: DC.x - DI, y: DC.y - DI, w: 2 * DI, h: 2 * DI }),
  k('right', '', 'nav', 'wedge-right', { x: DC.x + DI, y: DC.y - DR + 14, w: DR - DI, h: 2 * DR - 28 }),
  k('down', '', 'nav', 'wedge-down', { x: DC.x - DR + 14, y: DC.y + DI, w: 2 * DR - 28, h: DR - DI }),
  k('channel_up', '+', 'channel', 'rock-top', r(596, 572, 633, 622), { printed: 'Ch/Pg +' }),
  k('volume_down', '−', 'volume', 'rock-bottom', r(421, 626, 458, 676), { printed: 'Vol −' }),
  k('channel_down', '−', 'channel', 'rock-bottom', r(596, 626, 633, 676), { printed: 'Ch/Pg −' }),
  k('volume_mute', 'Mute', 'volume', 'rect', r(421, 688, 514, 713)),
  k(PK.return, '', 'channel', 'rect', r(541, 688, 633, 713), { printed: 'Back' }),
  k('dvr', 'DVR', 'guide', 'rect', r(421, 733, 489, 758)),
  k('guide', 'Guide', 'guide', 'rect', r(495, 733, 559, 758)),
  k('info', 'Info', 'guide', 'rect', r(565, 733, 633, 758)),
  k('red', 'Red', 'colour', 'pill', r(421, 774, 469, 790), { color: '#b8423a' }),
  k('green', 'Green', 'colour', 'pill', r(477, 774, 525, 790), { color: '#3e8e52' }),
  k('yellow', 'Yellow', 'colour', 'pill', r(529, 774, 577, 790), { color: '#bc9727' }),
  k('blue', 'Blue', 'colour', 'pill', r(585, 774, 633, 790), { color: '#3b69b8' }),
  k(PK.lightUpper, 'Light', 'home', 'rect', r(421, 807, 491, 836), { printed: 'upper light' }),
  k('home_control_plus', '+', 'home', 'rock-top', r(507, 807, 545, 838)),
  k(PK.socketUpper, 'Plug', 'home', 'rect', r(563, 807, 633, 836), { printed: 'upper socket' }),
  k(PK.lightLower, 'Light', 'home', 'rect', r(421, 845, 491, 874), { printed: 'lower light' }),
  k('home_control_minus', '−', 'home', 'rock-bottom', r(507, 842, 545, 874)),
  k(PK.socketLower, 'Plug', 'home', 'rect', r(563, 845, 633, 874), { printed: 'lower socket' }),
];

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
