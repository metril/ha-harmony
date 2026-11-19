// Pure logic of the Bluetooth views: error sentences, pairing countdown and outcome, signal bars, HID codes, the
// config edits (a `kind: bt` device, a `bt_remotes` entry) and the remote key table. No DOM; loaded with the views.
import { ApiError, type BtCodes, type BtHost, type BtRemote, type BtState, type BtStatus } from './api';
import { trimBytes } from './codes';
import { deviceIdFor } from './device-edit';
import type { Config, Device } from './model';

export type Tone = 'ok' | 'bad' | 'ir' | '';

// ------------------------------------------------------------------ errors
/** What the hub says when it refuses (`hub_refused`, `error.hub_code`) or the add-on does before asking (`invalid_bt`, `error.bt_code`). */
const HUB_CODES: Record<string, string> = {
  no_candidate: 'That remote is not in the list any more. Start the scan again and pick it again.',
  bad_addr: 'That address is not valid. It looks like AA:BB:CC:DD:EE:FF.',
  unsupported: "This hub's firmware cannot do that over Bluetooth.",
  bad_secs: 'The pairing time must be between 1 and 300 seconds.',
  bad_transport: 'That connection type is not supported.',
  bad_mode: 'That pairing mode is not supported.',
  bad_kind: 'That kind of device is not supported.',
  bad_page: 'That key type is not supported by this hub.',
  bad_action: 'That key action is not supported.',
  no_le: 'This hub has no Bluetooth LE. Choose Classic (or Any) instead.',
  no_slot: 'The hub has nothing paired in that place any more. The list will refresh; try again.',
  not_connected: 'The device is not connected right now. Wake it, make sure Bluetooth is on, and try again.',
  bt_not_ready: 'The hub’s Bluetooth is still starting. Wait a few seconds and try again.',
  bt_busy: 'The hub is busy with another Bluetooth task. Wait a moment and try again.',
  failed: 'The hub could not do that. Try again; if it keeps failing, restart the hub.',
};
const API_CODES: Record<string, string> = {
  bt_unknown: 'Home Assistant has not reported Bluetooth for this hub yet. Check that the Harmony integration is installed, up to date and connected.',
  hub_offline: 'The hub is offline. Check its power and network, then try again.',
  bt_unsupported: "This hub's firmware has no Bluetooth yet.",
  bt_busy: 'Too many Bluetooth requests are waiting. Wait a few seconds and try again.',
  hub_timeout: 'Home Assistant did not answer in time. Check that it is running and the Harmony integration is loaded, then try again.',
  rate_limited: 'Too many requests. Wait a moment and try again.',
  forbidden: 'The add-on refused that request. Reload the page and try again.',
  bad_request: 'The add-on could not understand that request. Reload the page and try again.',
  network: 'Cannot reach the add-on. Check your connection and try again.',
};
/** One plain sentence for any Bluetooth API failure; the code never is the main text. */
export function btError(e: unknown): string {
  if (!(e instanceof ApiError)) return 'Something went wrong. Try again.';
  const hubCode = typeof e.extra['hub_code'] === 'string' ? e.extra['hub_code'] : typeof e.extra['bt_code'] === 'string' ? e.extra['bt_code'] : '';
  if ((e.code === 'hub_refused' || e.code === 'invalid_bt') && HUB_CODES[hubCode]) return HUB_CODES[hubCode] as string;
  if (e.code === 'invalid_bt') return `${e.message.replace(/[.\s]*$/, '')}.`;
  if (e.code === 'hub_refused') return `The hub refused: ${e.message.replace(/[.\s]*$/, '')}. Try again.`;
  return API_CODES[e.code] ?? (e.isNetwork ? (API_CODES['network'] as string) : `The add-on could not do that (${e.status}). Try again.`);
}

/** One command id per user action. `id()` hands out the same one until `settle` says the action is over: a timeout or a lost
 *  connection keeps it (the retry joins the first request instead of running the command again), anything else drops it. */
export class CmdId {
  private cur: string | null = null;
  id(): string {
    if (!this.cur) {
      const a = 'abcdefghijklmnopqrstuvwxyz0123456789';
      this.cur = Array.from({ length: 12 }, () => a[Math.floor(Math.random() * a.length)]).join('');
    }
    return this.cur;
  }
  /** Call with the error (or nothing on success). */
  settle(e?: unknown): void {
    if (!(e instanceof ApiError) || !(e.isNetwork || e.code === 'hub_timeout')) this.cur = null;
  }
}

// ------------------------------------------------------------------ hub state
export const LINK_CHIP: Record<string, { tone: Tone; label: string; help: string }> = {
  off: { tone: '', label: 'Off', help: 'Bluetooth is idle. It starts by itself when you pair something or the config uses it.' },
  starting: { tone: 'ir', label: 'Starting', help: 'The hub is starting its Bluetooth. This takes a few seconds.' },
  ready: { tone: 'ok', label: 'Ready', help: 'Bluetooth is ready.' },
  no_adapter: { tone: 'bad', label: 'No adapter', help: 'The hub cannot find a Bluetooth adapter. Check the hardware; a restart may help.' },
  disabled: { tone: 'bad', label: 'Disabled', help: 'Bluetooth stopped after repeated failures. Restart the hub to try again.' },
};
export const BT_DOWN: Record<BtStatus['status'], { tone: Tone; label: string; title: string; body: string }> = {
  ok: { tone: 'ok', label: '', title: '', body: '' },
  unsupported: { tone: '', label: 'Not supported', title: "This hub's firmware has no Bluetooth yet", body: 'Update the hub firmware to a build with Bluetooth. Bluetooth devices you add to the config stay in the draft but the hub will refuse them until then.' },
  offline: { tone: 'ir', label: 'Hub offline', title: 'The hub is offline', body: 'Bluetooth settings come back when the hub does. Check its power and network, and that Home Assistant is running.' },
  unknown: { tone: 'ir', label: 'Not reported yet', title: 'Home Assistant has not reported Bluetooth yet', body: 'The Harmony integration has not told this editor about Bluetooth. Check that it is installed, version 0.3 or newer, and connected to this hub.' },
};

/** Paired hosts and remotes against the hub's limits: "2 of 8 devices, 1 of 4 remotes". */
export function limitText(st: BtStatus): string {
  const l = st.limits;
  if (!l) return '';
  const s = st.state;
  return `${s?.hosts.length ?? 0} of ${l.hosts_max} devices paired${l.remotes_max ? ` · ${s?.remotes.length ?? 0} of ${l.remotes_max} remotes` : ' · no remote input'}`;
}

// ------------------------------------------------------------------ pairing window
export interface Countdown {
  secs: number;
  at: number;
}
/** Project the hub's `discoverable_secs` forward with our own clock; adopt a new figure only when it drifts by 2 s or more. */
export function nextCountdown(prev: Countdown | null, state: BtState | null, now: number): Countdown | null {
  if (!state?.discoverable || state.discoverable_secs === undefined) return null;
  if (prev && Math.abs(secsLeft(prev, now) - state.discoverable_secs) < 2) return prev;
  return { secs: state.discoverable_secs, at: now };
}
export const secsLeft = (c: Countdown | null, now: number): number => (c ? Math.max(0, c.secs - Math.floor((now - c.at) / 1000)) : 0);
export const clock = (s: number): string => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

export interface Outcome {
  tone: Tone;
  title: string;
  body: string;
}
/** What a pairing result means and what to do next. `timeout` is ours: the window closed with nothing paired. */
export function outcome(r: 'ok' | 'failed' | 'refused' | 'timeout', what: 'device' | 'remote', name?: string): Outcome {
  if (r === 'ok') return { tone: 'ok', title: `Paired${name ? ` with ${name}` : ''}`, body: what === 'remote' ? 'Now choose what its buttons do.' : 'Now name it and choose its commands.' };
  if (r === 'refused')
    return {
      tone: 'bad',
      title: what === 'remote' ? 'The remote refused to pair' : 'The device refused to pair',
      body: what === 'remote' ? 'Put the remote in pairing mode again (usually hold Home and Back, or its pairing button, until its light blinks) and scan again.' : 'On the device, remove any old "Harmony" entry from its Bluetooth list, then start pairing here again and accept the request on the device.',
    };
  if (r === 'failed')
    return {
      tone: 'bad',
      title: 'Pairing did not finish',
      body: 'Stay close to the hub, make sure the other side is still in pairing mode, and try again. If it keeps failing, restart the hub.',
    };
  return {
    tone: 'ir',
    title: 'Nothing paired in time',
    body: what === 'remote' ? 'No remote was chosen before the window closed. Put the remote in pairing mode and scan again.' : 'The window closed before a device paired. Start again, then open Bluetooth settings on the device straight away.',
  };
}

/** 0..4 bars from a signal in dBm; -127 (unknown) is none. */
export function bars(rssi: number): number {
  return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi > -127 ? 1 : 0;
}

/** Hosts that were not there before the window opened. */
export const newHosts = (before: ReadonlySet<string>, state: BtState | null): BtHost[] => (state?.hosts ?? []).filter((h) => !before.has(h.addr));
export const newRemotes = (before: ReadonlySet<string>, state: BtState | null): BtRemote[] => (state?.remotes ?? []).filter((h) => !before.has(h.addr));

// ------------------------------------------------------------------ HID codes and command sets
const HID = /^HID:([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})$/;
export const hidCode = (page: number, usage: number): string => `HID:${page.toString(16).toUpperCase().padStart(2, '0')}:${usage.toString(16).toUpperCase().padStart(4, '0')}`;
/** `HID:0C:0223` -> {page, usage}, or a sentence saying what is wrong (the add-on's rules: pages 07 and 0C, usage ranges). */
export function parseHid(text: string): { page: number; usage: number } | string {
  const m = HID.exec(text.trim());
  if (!m) return 'Write it as HID:<page>:<usage> in hex, for example HID:0C:0223.';
  const page = parseInt(m[1] as string, 16);
  const usage = parseInt(m[2] as string, 16);
  if (page !== 7 && page !== 12) return 'The page must be 07 (keyboard) or 0C (consumer).';
  const max = page === 7 ? 0xff : 0x3ff;
  if (usage < 1 || usage > max) return `The usage must be 0001 to ${max.toString(16).toUpperCase().padStart(4, '0')} for page ${page === 7 ? '07' : '0C'}.`;
  return { page, usage };
}
/** The built-in name of a code, else its `HID:` form. */
export function nameOfUsage(codes: BtCodes | null, page: number, usage: number): string {
  return codes?.builtins.find((b) => b.page === page && b.usage === usage)?.name ?? hidCode(page, usage);
}
/** Resolve a command or key id (a built-in name in any case, or `HID:`) to its code, when we can. */
export function resolveId(codes: BtCodes, id: string): { page: number; usage: number } | null {
  const b = codes.builtins.find((x) => x.name.toLowerCase() === id.toLowerCase());
  if (b) return b;
  const h = parseHid(id);
  return typeof h === 'string' ? null : h;
}
export const searchBuiltins = (list: BtCodes['builtins'], q: string): BtCodes['builtins'] => {
  const s = q.trim().toLowerCase();
  return s ? list.filter((b) => b.name.toLowerCase().includes(s)) : list;
};
export const pageName = (page: number): string => (page === 7 ? 'Keyboard' : page === 12 ? 'Consumer' : `Page ${page}`);

// ------------------------------------------------------------------ config edits
/** usage -> key name; null removes a default (that button does nothing) */
export type KeyMap = Record<string, string | null>;
export interface BtRemoteCfg {
  name: string;
  address?: string;
  keys?: KeyMap;
}
export const remotesOf = (cfg: Config): BtRemoteCfg[] => ((cfg['bt_remotes'] as BtRemoteCfg[] | undefined) ?? []).filter((r) => r && typeof r === 'object');
export const btDevices = (cfg: Config): Device[] => (cfg.devices ?? []).filter((d) => d.kind === 'bt');
export const sameAddr = (a?: string, b?: string): boolean => !!a && !!b && a.toUpperCase() === b.toUpperCase();

export interface NewHost {
  name: string;
  address: string;
  transport?: 'auto' | 'classic' | 'le';
  commands: Record<string, string>;
}
/** Append a `kind: bt` device (explicit commands, so the editor shows them). */
export function addHostDevice(cfg: Config, h: NewHost): { config: Config; id: string } {
  const name = trimBytes(h.name.trim()) || 'Bluetooth device';
  const id = deviceIdFor(cfg, name);
  const dev = { id, kind: 'bt', name, address: h.address.toUpperCase(), transport: h.transport ?? 'auto', commands: h.commands } as unknown as Device;
  return { config: { ...cfg, devices: [...(cfg.devices ?? []), dev] }, id };
}
export function patchHost(cfg: Config, id: string, patch: Partial<Pick<Device, 'address' | 'transport' | 'name'>> & { commands?: Record<string, string> }): Config {
  return { ...cfg, devices: (cfg.devices ?? []).map((d) => (d.id === id ? ({ ...d, ...patch } as Device) : d)) };
}
export function addRemote(cfg: Config, r: BtRemoteCfg): Config {
  return { ...cfg, ['bt_remotes']: [...remotesOf(cfg), { ...r, ...(r.address ? { address: r.address.toUpperCase() } : {}) }] };
}
export function patchRemote(cfg: Config, index: number, patch: Partial<BtRemoteCfg>): Config {
  const list = remotesOf(cfg).map((r, i) => (i === index ? { ...r, ...patch } : r));
  const cur = list[index];
  if (cur?.keys && !Object.keys(cur.keys).length) list[index] = { name: cur.name, ...(cur.address ? { address: cur.address } : {}) };
  return { ...cfg, ['bt_remotes']: list };
}
export function removeRemote(cfg: Config, index: number): Config {
  const list = remotesOf(cfg).filter((_, i) => i !== index);
  const rest: Config = Object.fromEntries(Object.entries(cfg).filter(([k]) => k !== 'bt_remotes'));
  return list.length ? { ...rest, bt_remotes: list } : rest;
}

// ------------------------------------------------------------------ remote key table
export interface KeyRow {
  /** the config key: a built-in name or `HID:pp:uuuu` */
  id: string;
  page: number;
  usage: number;
  /** key the hub maps it to now; null: does nothing */
  key: string | null;
  /** default key of a built-in default row (undefined for rows of your own) */
  was?: string;
  mine: boolean;
}
/** Defaults first (with your override applied), then your own rows that are not a default's usage. */
export function keyRows(codes: BtCodes, keys: KeyMap | undefined): KeyRow[] {
  const mine = new Map<string, [string, string | null]>(); // "page:usage" -> [id, key]
  for (const [id, key] of Object.entries(keys ?? {})) {
    const c = resolveId(codes, id);
    mine.set(c ? `${c.page}:${c.usage}` : `?${id}`, [id, key]);
  }
  const rows: KeyRow[] = [];
  const seen = new Set<string>();
  for (const d of codes.remote_keys) {
    const k = `${d.page}:${d.usage}`;
    if (seen.has(k)) continue;
    seen.add(k);
    const o = mine.get(k);
    rows.push({ id: o?.[0] ?? d.name, page: d.page, usage: d.usage, key: o ? o[1] : d.key, was: d.key, mine: !!o && o[1] !== d.key });
  }
  for (const [k, [id, key]] of mine) {
    if (seen.has(k)) continue;
    if (key === null) continue;
    const c = resolveId(codes, id);
    rows.push({ id, page: c?.page ?? 0, usage: c?.usage ?? 0, key, mine: true });
  }
  return rows;
}
/** Set (or, when it equals the default, clear) the key of one row; returns the new `keys` map. */
export function setRowKey(keys: KeyMap | undefined, row: Pick<KeyRow, 'id' | 'was'>, key: string | null): KeyMap {
  const out = { ...(keys ?? {}) };
  if (row.was === undefined ? key === null : key === row.was) delete out[row.id];
  else out[row.id] = key;
  return out;
}
export const dropRowKey = (keys: KeyMap | undefined, id: string): KeyMap => {
  const out = { ...(keys ?? {}) };
  delete out[id];
  return out;
};
/** Why a key name cannot be used, or null: one of the hub's key names, or `key_<n>` below 128. */
export function keyProblem(codes: BtCodes, key: string): string | null {
  const k = key.trim();
  if (!k) return 'Choose which key it presses.';
  if (codes.keys.includes(k)) return null;
  const m = /^key_(\d{1,3})$/.exec(k);
  if (m && Number(m[1]) < 128) return null;
  return `"${k}" is not a key name. Pick one from the list, or use key_<number> up to key_127.`;
}
/** Is the code already mapped (default or yours)? */
export function isMapped(rows: readonly KeyRow[], page: number, usage: number): boolean {
  return rows.some((r) => r.page === page && r.usage === usage && r.key !== null);
}

// ------------------------------------------------------------------ key names for people
export const KEY_GROUPS = ['Navigation', 'Transport', 'Volume and channel', 'Colour', 'Digits', 'Other'] as const;
export type KeyGroup = (typeof KEY_GROUPS)[number];
const DIGIT = /^key_10([0-9])$/;
/** "Digit 0", "Volume up", "Key 5"; the id stays available as a tooltip. */
export function keyLabel(k: string): string {
  const d = DIGIT.exec(k);
  if (d) return `Digit ${d[1]}`;
  const n = /^key_(\d+)$/.exec(k);
  if (n) return `Key ${n[1]}`;
  const t = k.replace(/_/g, ' ');
  return t.charAt(0).toUpperCase() + t.slice(1);
}
export function keyGroup(k: string): KeyGroup {
  if (DIGIT.test(k)) return 'Digits';
  if (/^(volume|channel)_|^(mute)$/.test(k)) return 'Volume and channel';
  if (/^(red|green|yellow|blue)$/.test(k)) return 'Colour';
  if (/^(play|pause|stop|record|rewind|fast_forward|skip_|next|prev)/.test(k)) return 'Transport';
  if (/^(up|down|left|right|select|ok|exit|back|home|menu|guide|info|favorites|options)$/.test(k)) return 'Navigation';
  return 'Other';
}
/** The valid key names grouped for a picker; `extra` (a name already in use that is not listed) is kept so it shows. */
export function keyOptions(codes: BtCodes, extra?: string | null): { group: KeyGroup; keys: { id: string; label: string }[] }[] {
  const all = new Set(codes.keys);
  if (extra) all.add(extra);
  return KEY_GROUPS.map((g) => ({ group: g, keys: [...all].filter((k) => keyGroup(k) === g).map((id) => ({ id, label: keyLabel(id) })) })).filter((g) => g.keys.length);
}
/** "Up" for `up`, "Does nothing" for null. */
export const keyText = (k: string | null): string => (k === null ? 'Does nothing' : keyLabel(k));
