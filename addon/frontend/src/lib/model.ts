// Config shapes (docs/ha-config.md). Loose on purpose: the server validates.
export interface IrAction {
  proto?: string;
  addr?: number;
  cmd?: number;
  raw?: { hz: number; us: number[] };
  pronto?: string;
  flipper?: { file: string; signal?: string };
  smartir?: { file: string; command: string };
  port_mask?: number;
  /** 1..8 frames in total (needs `capabilities.ha_actions`) */
  repeats?: number;
  /** Home Assistant service action (HA-backed devices, keys, taps, steps) */
  ha?: HaCall;
}
export interface HaCall {
  service: string;
  target: { entity_id: string | string[] };
  data?: Record<string, unknown>;
  repeat?: boolean;
}
export interface Device {
  /** `ha`: no IR, power / inputs / commands are Home Assistant actions */
  kind?: 'ir' | 'ha';
  /** where an HA-backed device's state comes from */
  entity?: string;
  id: string;
  name?: string;
  aliases?: string[];
  settle_ms?: number;
  power?: { on?: IrAction; off?: IrAction; toggle?: IrAction };
  inputs?: Record<string, IrAction>;
  commands?: Record<string, IrAction>;
  source?: Record<string, string | number | boolean | null>;
}
export interface Activity {
  id: string;
  name?: string;
  devices?: { device: string; input?: string }[];
  steps?: Record<string, unknown>[];
  off?: Record<string, unknown>[];
  keys?: Record<string, unknown>;
}
export interface Widget {
  type: string;
  id?: string;
  x?: number;
  y?: number;
  w?: number;
  h?: number;
  label?: string;
  role?: string;
  shape?: string;
  icon?: string;
  icon_only?: boolean;
  /** `next`, `prev`, `home` or a page id: a navigation button */
  nav?: string;
  [k: string]: unknown;
}
export interface Page {
  id: string;
  cols?: number;
  rows?: number;
  kind?: 'home' | 'favorites';
  title?: string;
  activity?: string;
  back?: boolean;
  /** a list page takes its rows from here and places widgets itself */
  list?: { rows: number };
  widgets: Widget[];
}
export interface Config {
  devices?: Device[];
  activities?: Activity[];
  keys?: Record<string, unknown>;
  pages?: Page[];
  editor?: Record<string, unknown>;
  [k: string]: unknown;
}
export interface ValidationError {
  path: (string | number)[];
  msg: string;
  line?: number | null;
}

export const LIMITS = { devices: 16, activities: 16, commands: 256, inputs: 64, widgets: 128, jsonBytes: 128 * 1024 };

export function counts(c: Config | null) {
  const devices = c?.devices ?? [];
  return {
    devices: devices.length,
    activities: (c?.activities ?? []).length,
    commands: devices.reduce((n, d) => n + Object.keys(d.commands ?? {}).length, 0),
    inputs: devices.reduce((n, d) => n + Object.keys(d.inputs ?? {}).length, 0),
    widgets: (c?.pages ?? []).reduce((n, p) => n + (p.widgets ?? []).length, 0),
    pages: (c?.pages ?? []).length,
    keys: Object.keys(c?.keys ?? {}).length,
  };
}
