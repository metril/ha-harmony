// One canonical order for every key / command grid (palette groups, Free Keys, the device keypad, Bluetooth keys).
// Each row lists ids that belong on one line; a row of two is a pair that must share a row, left then right.
// Names are matched after normalising (case, separators, common aliases), so IR, Home Assistant and Bluetooth spellings agree.
export const ORDER: readonly (readonly string[])[] = [
  ['up', 'down'], ['left', 'right'], ['ok'], ['back', 'menu'], ['exit'], ['home'], ['guide', 'info'], ['options'],
  ['volume_up', 'volume_down'], ['mute'], ['channel_up', 'channel_down'], ['prev_channel'],
  ['play', 'pause'], ['play_pause'], ['rewind', 'fast_forward'], ['skip_back', 'skip_forward'], ['stop', 'record'],
  ['red', 'green'], ['yellow', 'blue'],
  ['1'], ['2'], ['3'], ['4'], ['5'], ['6'], ['7'], ['8'], ['9'], ['0'], ['enter'], ['dash'],
  ['power_on', 'power_off'], ['power_toggle'],
  ['home_control_1', 'home_control_2'], ['home_control_3', 'home_control_4'], ['home_control_plus', 'home_control_minus'],
];

const ALIAS: Record<string, string> = {
  vol_up: 'volume_up', vol_down: 'volume_down', volume_mute: 'mute', ch_up: 'channel_up', ch_down: 'channel_down', chan_up: 'channel_up', chan_down: 'channel_down',
  last: 'prev_channel', previous_channel: 'prev_channel', ffwd: 'fast_forward', ff: 'fast_forward', rew: 'rewind', fast_rewind: 'rewind',
  skip_next: 'skip_forward', next: 'skip_forward', next_track: 'skip_forward', skip_prev: 'skip_back', skip_previous: 'skip_back', prev: 'skip_back', previous: 'skip_back', previous_track: 'skip_back',
  select: 'ok', enter_ok: 'ok', return: 'back', direction_up: 'up', direction_down: 'down', direction_left: 'left', direction_right: 'right',
  dpad_up: 'up', dpad_down: 'down', dpad_left: 'left', dpad_right: 'right', dpad_center: 'ok', arrow_up: 'up', arrow_down: 'down', arrow_left: 'left', arrow_right: 'right',
  power: 'power_toggle', on: 'power_on', off: 'power_off', turn_on: 'power_on', turn_off: 'power_off', rec: 'record',
};
/** Normalise a key or command name to its order id: "VolUp", "vol_up" and "Volume Up" are all "volume_up". */
export function canonicalKey(name: string): string {
  const c = name.replace(/([a-z0-9])([A-Z])/g, '$1_$2').replace(/[\s-]+/g, '_').replace(/__+/g, '_').toLowerCase();
  const d = /^key_10([0-9])$/.exec(c) ?? /^(?:num|digit|number|key|btn|button)_?(\d)$/.exec(c);
  if (d) return d[1] as string;
  return ALIAS[c] ?? c;
}

const RANK = new Map<string, { row: number; col: number }>();
ORDER.forEach((r, row) => r.forEach((id, col) => RANK.set(id, { row, col })));
const natural = (a: string, b: string): number => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' });

export type Unit<T> = [T] | [T, T];

/** Sort by the canonical order; unknown names follow the known ones in natural order. */
export function orderBy<T>(items: readonly T[], nameOf: (t: T) => string): T[] {
  const key = (t: T) => {
    const id = canonicalKey(nameOf(t));
    const r = RANK.get(id);
    return { r, name: nameOf(t) };
  };
  return items.map((t) => ({ t, k: key(t) })).sort((a, b) => {
    const ra = a.k.r;
    const rb = b.k.r;
    if (ra && rb) return ra.row - rb.row || ra.col - rb.col;
    if (ra) return -1;
    if (rb) return 1;
    return natural(a.k.name, b.k.name);
  }).map((x) => x.t);
}

/** Ordered units: a pair of items whose ids share an order row come out as one unit, left then right. */
export function orderUnits<T>(items: readonly T[], nameOf: (t: T) => string): Unit<T>[] {
  const sorted = orderBy(items, nameOf);
  const out: Unit<T>[] = [];
  for (let i = 0; i < sorted.length; i++) {
    const a = sorted[i] as T;
    const b = sorted[i + 1];
    const ra = RANK.get(canonicalKey(nameOf(a)));
    const rb = b === undefined ? undefined : RANK.get(canonicalKey(nameOf(b)));
    if (b !== undefined && ra && rb && ra.row === rb.row && ra.col === 0 && rb.col === 1) {
      out.push([a, b]);
      i++;
    } else out.push([a]);
  }
  return out;
}
