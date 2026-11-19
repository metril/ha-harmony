// Pure edits of one device: commands, power, inputs, ports, repeats, naming rules. No DOM.
import { commandNames } from './bt';
import { utf8Bytes, NAME_MAX } from './codes';
import { LIMITS, type Config, type Device, type IrAction } from './model';

const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
export const isHaDevice = (d: Device): boolean => d.kind === 'ha';
export type PowerMode = 'both' | 'toggle' | 'none';

export function powerMode(d: Device): PowerMode {
  if (d.power?.toggle) return 'toggle';
  if (d.power?.on || d.power?.off) return 'both';
  return 'none';
}

/** Why `name` cannot be a command (or input) name of `d`, or null. `self` is the name being renamed. */
export function nameProblem(d: Device, name: string, self?: string, kind: 'command' | 'input' = 'command'): string | null {
  const n = name.trim();
  if (!n) return 'Give it a name';
  if (utf8Bytes(n) > NAME_MAX) return `Shorten by ${utf8Bytes(n) - NAME_MAX} byte${utf8Bytes(n) - NAME_MAX === 1 ? '' : 's'} (${NAME_MAX} bytes at most)`;
  if (kind === 'command' && (n === 'power_on' || n === 'power_off' || n.startsWith('input_'))) return 'power_on, power_off and input_… are reserved';
  const taken = Object.keys(kind === 'command' ? (d.commands ?? {}) : (d.inputs ?? {}));
  if (n !== self && taken.includes(n)) return 'Another one has this name';
  return null;
}

export function uniqueName(existing: string[], base: string): string {
  if (!existing.includes(base)) return base;
  let i = 2;
  while (existing.includes(`${base} ${i}`)) i++;
  return utf8Bytes(`${base} ${i}`) > NAME_MAX ? `${base.slice(0, NAME_MAX - 4)} ${i}` : `${base} ${i}`;
}

export function deviceIdFor(cfg: Config, base: string): string {
  const slug = base.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 24) || 'device';
  const used = new Set((cfg.devices ?? []).map((d) => d.id));
  if (!used.has(slug)) return slug;
  let i = 2;
  while (used.has(`${slug}_${i}`)) i++;
  return `${slug}_${i}`;
}

function edit(cfg: Config, id: string, fn: (d: Device) => void): Config {
  const next = clone(cfg);
  const d = (next.devices ?? []).find((x) => x.id === id);
  if (d) fn(d);
  return next;
}
export const patchDevice = (cfg: Config, id: string, patch: Partial<Device>): Config =>
  edit(cfg, id, (d) => {
    for (const [k, v] of Object.entries(patch)) {
      if (v === undefined || v === '') delete (d as unknown as Record<string, unknown>)[k];
      else (d as unknown as Record<string, unknown>)[k] = v;
    }
  });

export const setCommand = (cfg: Config, id: string, name: string, code: IrAction): Config =>
  edit(cfg, id, (d) => {
    d.commands = { ...(d.commands ?? {}), [name]: code };
  });
export function duplicateCommand(cfg: Config, id: string, name: string): { config: Config; name: string } {
  const d = (cfg.devices ?? []).find((x) => x.id === id);
  const code = d?.commands?.[name];
  if (!d || !code) return { config: cfg, name };
  const copy = uniqueName(Object.keys(d.commands ?? {}), `${name} copy`.slice(0, NAME_MAX));
  return { config: setCommand(cfg, id, copy, clone(code)), name: copy };
}
export const setInput = (cfg: Config, id: string, name: string, code: IrAction): Config =>
  edit(cfg, id, (d) => {
    d.inputs = { ...(d.inputs ?? {}), [name]: code };
  });

/** Set `port_mask` (1..7) or clear it (all ports) on a command, input or power code. */
export function withPorts(code: IrAction, mask: number | null): IrAction {
  const { port_mask: _drop, ...rest } = code;
  void _drop;
  return mask && mask !== 7 ? { ...rest, port_mask: mask } : rest;
}
export function withRepeats(code: IrAction, n: number): IrAction {
  const { repeats: _drop, ...rest } = code;
  void _drop;
  return n > 1 ? { ...rest, repeats: Math.min(8, Math.max(1, Math.round(n))) } : rest;
}
/** Every IR code of a device, with a path to rewrite it. */
export function codeSlots(d: Device): { where: 'power' | 'inputs' | 'commands'; name: string; code: IrAction }[] {
  const out: { where: 'power' | 'inputs' | 'commands'; name: string; code: IrAction }[] = [];
  for (const k of ['on', 'off', 'toggle'] as const) if (d.power?.[k]) out.push({ where: 'power', name: k, code: d.power[k] as IrAction });
  for (const [n, c] of Object.entries(d.inputs ?? {})) out.push({ where: 'inputs', name: n, code: c });
  for (const [n, c] of Object.entries(d.commands ?? {})) out.push({ where: 'commands', name: n, code: c });
  return out;
}
/** Apply `fn` to the chosen codes (all when `names` is null): bulk set IR ports, repeats. */
export function mapCodes(cfg: Config, id: string, names: string[] | null, fn: (c: IrAction) => IrAction): Config {
  return edit(cfg, id, (d) => {
    for (const [n, c] of Object.entries(d.commands ?? {})) if (!names || names.includes(n)) (d.commands as Record<string, IrAction>)[n] = fn(c);
  });
}

/** Switch the power model. Keeps the codes that make sense: toggle <- on, both <- toggle for both. */
export function setPowerMode(cfg: Config, id: string, mode: PowerMode): Config {
  return edit(cfg, id, (d) => {
    const cur = d.power ?? {};
    if (mode === 'none') delete d.power;
    else if (mode === 'toggle') d.power = { toggle: cur.toggle ?? cur.on ?? cur.off ?? ({ proto: 'nec', addr: 0, cmd: 0 } as IrAction) };
    else d.power = { on: cur.on ?? cur.toggle ?? ({ proto: 'nec', addr: 0, cmd: 0 } as IrAction), off: cur.off ?? cur.toggle ?? ({ proto: 'nec', addr: 0, cmd: 0 } as IrAction) };
  });
}
export const setPowerCode = (cfg: Config, id: string, which: 'on' | 'off' | 'toggle', code: IrAction | null): Config =>
  edit(cfg, id, (d) => {
    const p = { ...(d.power ?? {}) };
    if (code) p[which] = code;
    else delete p[which];
    if (Object.keys(p).length) d.power = p;
    else delete d.power;
  });

/** Status line of the power model for cards. */
export function powerStatus(d: Device): { text: string; tone: 'ok' | 'ir' | 'none' } {
  const p = d.power;
  if (d.kind === 'bt') return { text: 'Bluetooth', tone: 'none' };
  if (p?.on && p.off) return { text: 'Power on + off', tone: 'ok' };
  if (p?.toggle) return { text: 'Power toggle only', tone: 'ir' };
  if (p?.on || p?.off) return { text: 'Half a power pair', tone: 'ir' };
  return { text: 'No power control', tone: 'none' };
}

/** Where the device came from, for the source chip. */
export function sourceLabel(d: Device): string {
  if (isHaDevice(d)) return 'Home Assistant';
  if (d.kind === 'bt') return d.address ? `Bluetooth · ${d.address}` : 'Bluetooth · not paired yet';
  const s = d.source;
  if (s?.['kind'] === 'harmony_archive') return `IR archive · ${String(s['mfr'] ?? '')} ${String(s['model_file'] ?? '').replace(/\.json$/, '')}`.trim();
  if (s?.['kind']) return `Imported · ${String(s['kind'])}`;
  return 'Your Harmony backup';
}

export function commandCount(d: Device): number {
  return commandNames(d).length;
}

/** Total commands / inputs of the devices the hub compiles (Home Assistant devices are not). Bluetooth commands count like the
 *  backend's `config_stats` does (a bt device without `commands` counts its default set), so the editor never allows more
 *  than the hub accepts. */
export function irTotals(cfg: Config): { commands: number; inputs: number } {
  const ir = (cfg.devices ?? []).filter((d) => !isHaDevice(d));
  return { commands: ir.reduce((n, d) => n + commandNames(d).length, 0), inputs: ir.reduce((n, d) => n + Object.keys(d.inputs ?? {}).length, 0) };
}
/** Why adding `n` more of this kind would pass the hub's limit (compiler MAX_*), or null. `ha`: the target is a Home Assistant device. */
export function capProblem(cfg: Config, kind: 'command' | 'input' | 'device', n = 1, ha = false): string | null {
  if (kind === 'device') return (cfg.devices ?? []).length + n > LIMITS.devices ? `The hub holds at most ${LIMITS.devices} devices.` : null;
  if (ha) return null;
  const t = irTotals(cfg);
  if (kind === 'command') return t.commands + n > LIMITS.commands ? `The hub holds at most ${LIMITS.commands} commands across all devices (${t.commands} used). Delete some first.` : null;
  return t.inputs + n > LIMITS.inputs ? `The hub holds at most ${LIMITS.inputs} inputs across all devices (${t.inputs} used). Delete some first.` : null;
}
