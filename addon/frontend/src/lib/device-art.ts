// Device-type illustrations (64x48, 2 stroke, inline SVG) and the guesses that pick one for a device;
// plus the small function icons of command chips.
import { svg, type TemplateResult } from 'lit';
import type { Device } from './model';
import { groupCommand, type Group } from './studio-model';
import { stockIconByName } from './stock-icons';
import { bestIcon } from './icon-suggest';

export type Kind = 'tv' | 'receiver' | 'soundbar' | 'streamer' | 'console' | 'projector' | 'disc' | 'other';
export const KIND_LABEL: Record<Kind, string> = { tv: 'TV', receiver: 'AV receiver', soundbar: 'Soundbar', streamer: 'Streaming Box', console: 'Game Console', projector: 'Projector', disc: 'Blu-ray / DVD', other: 'Something Else' };
/** archive device type id the power-test and model list use for each kind */
export const KIND_TYPE: Record<Kind, string> = { tv: '1', receiver: '15', soundbar: '16', streamer: '14', console: '13', projector: '17', disc: '4', other: '14' };
/** the archive's other device types, for "Something else" */
export const OTHER_TYPES: [string, string][] = [['11', 'Cable box'], ['10', 'Satellite'], ['12', 'PVR / DVR'], ['3', 'CD player'], ['7', 'Tuner'], ['2', 'VCR'], ['21', 'Audio switcher'], ['22', 'Video switcher'], ['16', 'Amplifier'], ['14', 'Media player']];

const RULES: [RegExp, Kind][] = [
  [/soundbar|sound bar|sonos|homepod/i, 'soundbar'],
  [/receiver|avr|amp|denon|marantz|onkyo|yamaha rx|nad |pioneer vsx/i, 'receiver'],
  [/projector|beamer/i, 'projector'],
  [/blu.?ray|dvd|disc|bdp|cd player/i, 'disc'],
  [/xbox|playstation|ps[345]|console|switch|nintendo/i, 'console'],
  [/stream|shield|roku|apple tv|chromecast|fire ?tv|box|kodi|media player|nvidia/i, 'streamer'],
  [/tv|television|bravia|oled|qled|samsung|lg |panasonic|display|monitor/i, 'tv'],
];
export function deviceKind(d: Pick<Device, 'id' | 'name' | 'entity'>): Kind {
  const text = `${d.name ?? ''} ${d.id} ${d.entity ?? ''}`;
  return RULES.find(([re]) => re.test(text))?.[1] ?? 'other';
}

const P = (d: string) => svg`<path d=${d}></path>`;
const ART: Record<Kind, TemplateResult> = {
  tv: svg`<rect x="6" y="6" width="52" height="30" rx="3"></rect><path d="M24 42h16M32 36v6"></path><path d="M12 30l10-9 8 6 8-5 12 8" opacity=".5"></path>`,
  receiver: svg`<rect x="4" y="12" width="56" height="26" rx="3"></rect><circle cx="16" cy="25" r="6"></circle><circle cx="48" cy="25" r="6"></circle><path d="M26 22h12M26 28h12M8 42v2M56 42v2"></path>`,
  soundbar: svg`<rect x="3" y="18" width="58" height="14" rx="7"></rect><circle cx="14" cy="25" r="3"></circle><circle cx="50" cy="25" r="3"></circle><path d="M24 25h16M10 38v4M54 38v4"></path>`,
  streamer: svg`<rect x="10" y="14" width="44" height="22" rx="4"></rect><path d="M28 22l8 3-8 3z"></path><path d="M16 40h32"></path>`,
  console: svg`<rect x="6" y="14" width="30" height="24" rx="3"></rect><path d="M12 22h18M12 28h8"></path><rect x="42" y="20" width="16" height="14" rx="6"></rect><path d="M47 27h6M50 24v6"></path>`,
  projector: svg`<rect x="4" y="16" width="48" height="20" rx="3"></rect><circle cx="40" cy="26" r="6"></circle><path d="M56 20l6-4M56 26h6M56 32l6 4M12 40v3M44 40v3"></path>`,
  disc: svg`<rect x="4" y="16" width="56" height="18" rx="3"></rect><path d="M12 25h20"></path><circle cx="46" cy="25" r="5"></circle><path d="M8 38v3M56 38v3"></path>`,
  other: svg`<rect x="8" y="10" width="48" height="28" rx="4"></rect>${P('M24 24h16M32 16v16')}`,
};
import { iconInner } from './icon-shapes';
/** The device's own icon when it has one, else the drawing guessed from its name. */
export const deviceArt = (d: Pick<Device, 'id' | 'name' | 'entity' | 'icon'> | undefined, cls = 'art'): TemplateResult => {
  const ic = d?.icon ? stockIconByName(d.icon) : undefined;
  return ic
    ? svg`<svg class=${cls} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${iconInner(ic.id)}</svg>`
    : art(d ? deviceKind(d) : 'other', cls);
};
export const art = (k: Kind, cls = 'art'): TemplateResult =>
  svg`<svg class=${cls} viewBox="0 0 64 48" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ART[k]}</svg>`;

// ---------------------------------------------------------------- command icons
const NAME_ICON: [RegExp, string][] = [
  [/^power|^standby|^(turn_)?(on|off)$/, 'power'], [/mute/, 'mute'], [/vol.*(up|plus)|volume_up|^vol\+|vup/, 'vol_up'], [/vol.*(down|minus)|volume_down|^vol-|vdown/, 'vol_down'],
  [/chan.*(up|plus)|^ch_?up|^ch\+/, 'ch'], [/chan.*(down|minus)|^ch_?down|^ch-/, 'ch'],
  [/^play_?pause$/, 'play'], [/^play/, 'play'], [/^pause/, 'pause'], [/^stop/, 'stop'], [/^rec/, 'record'], [/^(rew|backward|reverse)/, 'rewind'], [/^(ff|fwd|forward|fast_forward)/, 'ffwd'],
  [/^next|skip_?next/, 'skip_next'], [/^prev|skip_?prev/, 'skip_prev'], [/^menu|^top_menu|^popup/, 'menu'], [/^guide/, 'guide'], [/^info/, 'info'], [/^home/, 'home'], [/^(back|exit|return)/, 'back'],
  [/^up$/, 'up'], [/^down$/, 'down'], [/^left$/, 'chev_left'], [/^right$/, 'chev_right'], [/^(ok|select|enter)$/, 'check'], [/^(input|source|hdmi|av|tv_audio)/, 'input'], [/^(cc|subtitle)/, 'cc'], [/^(setup|settings|options|tools)/, 'settings'],
];
const GROUP_ICON: Record<Group, string> = { power: 'power', volume: 'audio', navigation: 'menu', transport: 'play', numbers: 'list', inputs: 'input', other: 'settings' };
/** Stock icon id for a command: by its name, else by its function group. */
export function commandIconId(name: string, group: Group = groupCommand(name)): number {
  const n = name.toLowerCase();
  const hit = NAME_ICON.find(([re]) => re.test(n))?.[1];
  return stockIconByName(hit ?? GROUP_ICON[group])?.id ?? 0;
}
/** Palette chip icon: what the compiler would draw for the command (icon-suggest), else the name table / group icon. */
const inferred = new Map<string, number>();
export const chipIconId = (name: string, group: Group = groupCommand(name)): number => {
  let id = inferred.get(name);
  if (id === undefined) inferred.set(name, (id = stockIconByName(bestIcon(name))?.id ?? 0));
  return id || commandIconId(name, group);
};
export const groupIconId = (g: Group): number => stockIconByName(GROUP_ICON[g])?.id ?? 0;
