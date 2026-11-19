// The part of Bluetooth the whole editor needs (palette, action picker, key assignment). The views and their
// logic live in bt-model.ts and views/hh-bluetooth.ts, which load only when opened.
import type { Api } from './api';
import type { Config, Device } from './model';

let defaults: string[] = [];
/** A `kind: bt` device without `commands` has the default command set; the names come from the add-on (`bt/codes`). */
export function setBtDefaults(names: string[]): void {
  defaults = names;
}
export const isBtDevice = (d: Pick<Device, 'kind'>): boolean => d.kind === 'bt';
/** The command names of a device: its own, or for a Bluetooth device that lists none, the default set. */
export function commandNames(d: Device): string[] {
  return d.commands ? Object.keys(d.commands) : d.kind === 'bt' ? defaults : [];
}
/** Fetch the default set once when the config holds a Bluetooth device that relies on it. `done` runs after it arrived. */
export async function primeBtDefaults(api: Api, cfg: Config, done: () => void): Promise<void> {
  if (defaults.length || !(cfg.devices ?? []).some((d) => d.kind === 'bt' && !d.commands)) return;
  try {
    setBtDefaults((await api.btCodes()).builtins.filter((b) => b.default).map((b) => b.name));
    done();
  } catch {
    /* the add-on is older: such a device simply shows no commands */
  }
}
