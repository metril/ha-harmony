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
