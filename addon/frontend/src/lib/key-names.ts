// One place that turns a key name (wire/config spelling: snake_case, CamelCase or key_<n>) into the text a person reads.
// Never changes the stored value. Explicit table first, then a generic splitter that keeps known acronyms.
const NAMES: Record<string, string> = {
  home_control_1: 'Upper bulb', home_control_2: 'Lower bulb', home_control_3: 'Upper socket', home_control_4: 'Lower socket',
  home_control_plus: 'Home control plus', home_control_minus: 'Home control minus',
  prev_channel: 'Previous channel', power_off_activity: 'Power off activity',
  select: 'OK', ok: 'OK', dvr: 'DVR', tv: 'TV', fast_forward: 'Fast forward',
};
const ACRONYMS = new Set(['ok', 'dvr', 'tv', 'hdmi', 'usb', 'ir', 'av', 'dvd', 'cd', 'hd', 'epg', 'ui']);
const DIGIT = /^key_10([0-9])$/;
const canon = (k: string): string => k.replace(/([a-z0-9])([A-Z])/g, '$1_$2').replace(/([A-Za-z])(\d)/g, '$1_$2').replace(/[\s-]+/g, '_').toLowerCase();

/** "Volume down", "Previous channel", "OK", "Upper bulb", "5" for Num5 / key_105, "Key 7" for key_7. */
export function displayKey(name: string): string {
  const c = canon(name);
  const hit = NAMES[c];
  if (hit) return hit;
  const d = DIGIT.exec(c) ?? /^(?:num|digit|number)_?(\d)$/.exec(c);
  if (d) return d[1] as string;
  const n = /^key_(\d+)$/.exec(c);
  if (n) return `Key ${n[1]}`;
  const words = c.split('_').filter(Boolean).map((w) => (ACRONYMS.has(w) ? w.toUpperCase() : w));
  if (!words.length) return name;
  const t = words.join(' ');
  return t.charAt(0).toUpperCase() + t.slice(1);
}

const CMD_WORDS: Record<string, string> = { vol: 'volume', ch: 'channel', chan: 'channel', pwr: 'power', prev: 'previous' };
/** A device command or input name for people: "Power on", "Volume up", "HDMI 1", "OK". The config name is untouched. */
export function displayCommand(name: string): string {
  const c = canon(name);
  const hit = NAMES[c];
  if (hit) return hit;
  const m = /^hdmi_?(\d+)$/.exec(c);
  if (m) return `HDMI ${m[1]}`;
  return displayKey(c.split('_').map((w) => CMD_WORDS[w] ?? w).join('_'));
}

/** An equal-column grid sized to the longest label, so every cell is as wide as the widest word needs and nothing truncates. */
export const gridStyle = (labels: readonly string[], pad = '2.6rem', cap = Infinity): string => {
  const n = Math.min(cap, Math.max(4, ...labels.map((l) => l.length)));
  return `repeat(auto-fill, minmax(min(100%, calc(${n}ch + ${pad})), 1fr))`;
};
/** Labels longer than the column width a capped grid is sized to take two columns (rows stay aligned, nothing is cut). */
export const FREE_CAP = 9;

/** The palette's column width: labels up to this many characters stay one cell, longer ones take two. */
export const PALETTE_CAP = 12;
