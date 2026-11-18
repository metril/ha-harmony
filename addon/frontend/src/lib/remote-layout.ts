// Geometry of the Harmony Elite drawing, in one place. Everything is in a 300x900 viewBox.
// Positions follow docs/hardware/remote-elite.md (key list) and are drawn from the key list and memory,
// not measured: correct them here against a photo or the bench device. Keys are keyed by the
// backend's snake_case key names (proto/schema.yaml), checked by test/layout.test.ts.
export const VIEW = { w: 300, h: 900 };
export const BODY = { x: 8, y: 8, w: 284, h: 884, r: 46 };
export const GLASS = { x: 20, y: 42, w: 260, h: 346, r: 18 };
/** the 240x320 display */
export const SCREEN = { x: 30, y: 56, w: 240, h: 320 };

export type KeyShape = 'rect' | 'pill' | 'circle' | 'wedge-up' | 'wedge-down' | 'wedge-left' | 'wedge-right';
export interface KeyGeom {
  name: string;
  /** stock spelling, for tooltips */
  stock: string;
  label: string;
  group: string;
  shape: KeyShape;
  x: number;
  y: number;
  w: number;
  h: number;
  /** colour keys */
  color?: string;
}

const k = (name: string, stock: string, label: string, group: string, shape: KeyShape, x: number, y: number, w: number, h: number, color?: string): KeyGeom => ({ name, stock, label, group, shape, x, y, w, h, color });

export const KEYS: readonly KeyGeom[] = [
  k('home', 'Home', 'Home', 'strip', 'pill', 30, 394, 112, 28),
  k('favorites', 'Favorites', 'Favorites', 'strip', 'pill', 158, 394, 112, 28),
  k('menu', 'Menu', 'Menu', 'menu', 'pill', 32, 436, 62, 26),
  k('exit', 'Exit', 'Exit', 'menu', 'pill', 206, 436, 62, 26),
  k('up', 'Up', 'Up', 'nav', 'wedge-up', 118, 440, 64, 34),
  k('left', 'Left', 'Left', 'nav', 'wedge-left', 84, 478, 36, 62),
  k('select', 'Select', 'OK', 'nav', 'circle', 124, 486, 52, 52),
  k('right', 'Right', 'Right', 'nav', 'wedge-right', 180, 478, 36, 62),
  k('down', 'Down', 'Down', 'nav', 'wedge-down', 118, 544, 64, 34),
  k('volume_up', 'VolumeUp', 'Vol +', 'volume', 'rect', 32, 596, 62, 38),
  k('volume_mute', 'VolumeMute', 'Mute', 'volume', 'rect', 32, 638, 62, 26),
  k('volume_down', 'VolumeDown', 'Vol -', 'volume', 'rect', 32, 668, 62, 38),
  k('guide', 'Guide', 'Guide', 'guide', 'rect', 118, 596, 64, 30),
  k('info', 'Info', 'Info', 'guide', 'rect', 118, 634, 64, 30),
  k('dvr', 'DVR', 'DVR', 'guide', 'rect', 118, 672, 64, 30),
  k('channel_up', 'ChannelUp', 'Ch +', 'channel', 'rect', 206, 596, 62, 38),
  k('prev_channel', 'PrevChannel', 'Prev', 'channel', 'rect', 206, 638, 62, 26),
  k('channel_down', 'ChannelDown', 'Ch -', 'channel', 'rect', 206, 668, 62, 38),
  k('rewind', 'Rewind', 'Rew', 'transport', 'rect', 32, 722, 72, 30),
  k('play', 'Play', 'Play', 'transport', 'rect', 114, 722, 72, 30),
  k('fast_forward', 'FastForward', 'FF', 'transport', 'rect', 196, 722, 72, 30),
  k('record', 'Record', 'Rec', 'transport', 'rect', 32, 758, 72, 30),
  k('pause', 'Pause', 'Pause', 'transport', 'rect', 114, 758, 72, 30),
  k('stop', 'Stop', 'Stop', 'transport', 'rect', 196, 758, 72, 30),
  k('red', 'Red', 'Red', 'colour', 'pill', 32, 802, 56, 16, '#b8423a'),
  k('green', 'Green', 'Green', 'colour', 'pill', 94, 802, 56, 16, '#3e8e52'),
  k('yellow', 'Yellow', 'Yellow', 'colour', 'pill', 156, 802, 56, 16, '#bc9727'),
  k('blue', 'Blue', 'Blue', 'colour', 'pill', 218, 802, 50, 16, '#3b69b8'),
  k('home_control_1', 'HomeControl1', '1', 'home', 'rect', 32, 830, 56, 26),
  k('home_control_2', 'HomeControl2', '2', 'home', 'rect', 94, 830, 56, 26),
  k('home_control_3', 'HomeControl3', '3', 'home', 'rect', 156, 830, 56, 26),
  k('home_control_4', 'HomeControl4', '4', 'home', 'rect', 218, 830, 50, 26),
  k('home_control_minus', 'HomeControlMinus', '-', 'home', 'pill', 32, 862, 56, 22),
  k('power_off_activity', 'PowerOffActivity', 'Off', 'power', 'pill', 106, 862, 88, 22, '#7a2b27'),
  k('home_control_plus', 'HomeControlPlus', '+', 'home', 'pill', 212, 862, 56, 22),
];

export const keyByName = new Map(KEYS.map((g) => [g.name, g]));
/** "Volume Up" style name for tooltips and announcements */
export const keyTitle = (name: string): string => keyByName.get(name)?.stock.replace(/([a-z0-9])([A-Z])/g, '$1 $2') ?? name;

/** SVG path for a key shape in its own box (x,y,w,h). */
export function keyPath(g: KeyGeom): string {
  const { x, y, w, h } = g;
  const rr = (r: number) => `M${x + r} ${y}h${w - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}v${h - 2 * r}a${r} ${r} 0 0 1 -${r} ${r}h-${w - 2 * r}a${r} ${r} 0 0 1 -${r} -${r}v-${h - 2 * r}a${r} ${r} 0 0 1 ${r} -${r}z`;
  switch (g.shape) {
    case 'circle': {
      const r = w / 2;
      return `M${x} ${y + r}a${r} ${r} 0 1 0 ${w} 0a${r} ${r} 0 1 0 -${w} 0z`;
    }
    case 'pill':
      return rr(h / 2);
    case 'rect':
      return rr(8);
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
