// One place that turns a key name (wire/config spelling: snake_case, CamelCase or key_<n>) into the text a person reads.
// Never changes the stored value. Explicit table first, then a generic splitter that keeps known acronyms and codes.
// Names are Title Case; small joining words stay lower-case unless first.
const NAMES: Record<string, string> = {
  home_control_1: 'Upper Bulb', home_control_2: 'Lower Bulb', home_control_3: 'Upper Socket', home_control_4: 'Lower Socket',
  home_control_plus: 'Home Control +', home_control_minus: 'Home Control −',
  prev_channel: 'Previous Channel', power_off_activity: 'Power Off Activity',
  select: 'OK', ok: 'OK', dvr: 'DVR', tv: 'TV', fast_forward: 'Fast Forward', play_pause: 'Play/Pause',
  arrowup: 'Arrow Up', arrowdown: 'Arrow Down', arrowleft: 'Arrow Left', arrowright: 'Arrow Right', arrow_up: 'Arrow Up', arrow_down: 'Arrow Down', arrow_left: 'Arrow Left', arrow_right: 'Arrow Right',
  capslock: 'Caps Lock', caps_lock: 'Caps Lock',
};
/** What a screen reader and the palette search get for names that are printed as a symbol. */
const SPOKEN: Record<string, string> = { home_control_plus: 'Home Control Plus', home_control_minus: 'Home Control Minus' };
const ACRONYMS = new Set(['ok', 'dvr', 'tv', 'hdmi', 'usb', 'ir', 'av', 'dvd', 'cd', 'hd', 'uhd', 'sd', 'epg', 'ui', 'hdr', 'arc', 'pip', 'bt', 'ha', 'am', 'fm', 'cec', 'rf', 'led', 'lcd', 'osd', 'pc', 'vcr', 'pvr', 'nec', 'sirc', 'dts', 'cc', 'vod', 'kvm']);
const BRAND: Record<string, string> = { earc: 'eARC', dpad: 'D-Pad', necext: 'NECext' };
/** Joining words that stay lower-case in a Title Case name unless they come first. */
export const SMALL_WORDS = new Set(['a', 'an', 'and', 'of', 'the', 'to', 'in', 'on', 'at', 'for']);
// in a command or key name "On"/"In" are the verb ("Power On", "Zoom In"), so they stay capitalised there
const NAME_SMALL = new Set([...SMALL_WORDS].filter((w) => w !== 'on' && w !== 'in'));
// a letters+digits token splits ("HDMI1" -> "HDMI 1", "Input1" -> "Input 1") only when its letters are an acronym, a known word or 4+ long; short ones are codes ("A1", "F12", "B2")
const SPLIT_WORDS = new Set(['ch', 'vol', 'num', 'key', 'input', 'digit', 'page', 'mode', 'scene']);
const UNITS = new Set(['s', 'ms', 'min', 'sec', 'h', 'hz', 'khz', 'mhz', 'db', 'st', 'nd', 'rd', 'th', 'm', 'cm', 'mm']);
const DIGIT = /^key_10([0-9])$/;
const lookup = (k: string): string => k.replace(/([a-z0-9])([A-Z])/g, '$1_$2').replace(/([A-Za-z])(\d)/g, '$1_$2').replace(/[\s-]+/g, '_').toLowerCase();

/** Short forms spelled out wherever a name is shown ("vol_up" -> "Volume Up"). */
const CMD_WORDS: Record<string, string> = { vol: 'volume', ch: 'channel', chan: 'channel', pwr: 'power', prev: 'previous', pct: 'percent' };
const bare = (t: string): boolean => /^[a-z]+[+-]*$/.test(t);
/** Break a name into words. A token that is a code (letter+digit or digit+letter run: A1, 1A, 4K, F12) stays one uppercase token. */
function tokens(name: string): string[] {
  const out: string[] = [];
  for (const raw of name.split(/_+|\s+|(?<=[A-Za-z0-9])-(?=[A-Za-z0-9])/).filter(Boolean)) {
    const sym = /[+-]+$/.exec(raw)?.[0] ?? '';
    const part = sym ? raw.slice(0, -sym.length) : raw;
    if (!part) { out.push(raw); continue; }
    const pieces = /^[a-z][A-Z]/.test(part) ? [part] : part
      .replace(/([a-z])([A-Z])/g, '$1 $2').replace(/([A-Z]+)([A-Z][a-z])/g, '$1 $2').replace(/([A-Za-z]\d+)(?=[A-Z][a-z])/g, '$1 ').split(' ');
    for (const piece of pieces) {
      const low = piece.toLowerCase();
      const ld = /^([A-Za-z]+)(\d+)$/.exec(piece);
      const dl = /^(\d+)([A-Za-z]+)$/.exec(piece);
      if (BRAND[low]) out.push(BRAND[low] as string);
      else if (/^[a-z][A-Z]/.test(piece)) out.push(piece);
      else if (ld) {
        const l = (ld[1] as string).toLowerCase();
        if (ACRONYMS.has(l) || SPLIT_WORDS.has(l) || l.length >= 4) out.push(ACRONYMS.has(l) ? l.toUpperCase() : l, ld[2] as string);
        else out.push(piece.toUpperCase());
      } else if (dl) {
        const l = (dl[2] as string).toLowerCase();
        if (UNITS.has(l)) out.push(piece.toLowerCase());
        else if (l.length <= 2) out.push(piece.toUpperCase());
        else out.push(dl[1] as string, l);
      } else if (ACRONYMS.has(low)) out.push(low.toUpperCase());
      else out.push(low);
    }
    if (sym) out[out.length - 1] += sym;
  }
  return out.map((t) => CMD_WORDS[t] ?? t);
}
const cap = (t: string): string => t.charAt(0).toUpperCase() + t.slice(1);
/** Title Case a list of words: plain lower-case words get a capital, codes/acronyms stay, small joining words stay lower unless first. */
const title = (words: string[]): string => words.map((w, i) => (!bare(w) ? w : i > 0 && NAME_SMALL.has(w) ? w : cap(w))).join(' ');
/** Already written for people (has a space): shown as written, only tidied. */
const isHuman = (n: string): boolean => /\S\s+\S/.test(n.trim());

/** "Volume Down", "Previous Channel", "OK", "Input A1", "Upper Bulb", "5" for Num5 / key_105, "Key 7" for key_7. */
export function displayKey(name: string): string {
  if (/[:.]/.test(name) && !isHuman(name)) return name; // ids and codes ("light.lamp", "HID:0C:0223") are shown as they are
  if (name.includes('/') && !isHuman(name)) return name.split('/').map(displayKey).join('/');
  if (isHuman(name)) return cap(name.trim().replace(/\s+/g, ' '));
  const c = lookup(name);
  const hit = NAMES[c];
  if (hit) return hit;
  const d = DIGIT.exec(c) ?? /^(?:num|digit|number)_?(\d)$/.exec(c);
  if (d) return d[1] as string;
  const n = /^key_(\d+)$/.exec(c);
  if (n) return `Key ${n[1]}`;
  if (/^[A-Za-z]+\+[A-Za-z0-9]+$/.test(name)) return name.split('+').map((x) => (x.length <= 1 ? x.toUpperCase() : cap(x.toLowerCase()))).join('+');
  const words = tokens(name);
  return words.length ? title(words) : name;
}

/** The spoken form of a key name for aria-labels and titles ("Home Control Plus" where displayKey shows "Home Control +"). */
export const spokenKey = (name: string): string => SPOKEN[lookup(name)] ?? displayKey(name);

/** A device command or input name for people: "Power On", "Volume Up", "HDMI 1", "OK". The config name is untouched. */
export function displayCommand(name: string): string {
  if (isHuman(name) || /[:./]/.test(name)) return displayKey(name);
  const hit = NAMES[lookup(name)];
  if (hit) return hit;
  return displayKey(name);
}

/** An equal-column grid sized to the longest label, so every cell is as wide as the widest word needs and nothing truncates. */
export const gridStyle = (labels: readonly string[], pad = '2.6rem', cap = Infinity): string => {
  const n = Math.min(cap, Math.max(4, ...labels.map((l) => l.length)));
  return `repeat(auto-fill, minmax(min(100%, calc(${n}ch + ${pad})), 1fr))`;
};
/** Labels longer than the column width a capped grid is sized to take two columns (rows stay aligned, nothing is cut). */
export const FREE_CAP = 9;

/** The palette's cell fits a label (plus icon and padding) of this many characters; longer ones take two cells. Same for every group. */
export const PALETTE_CAP = 9;
