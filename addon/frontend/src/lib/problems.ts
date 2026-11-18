// Server validation errors ({path, msg}) turned into readable, located problems for the
// device and activity pages. The message text always comes from the backend; only the label
// ("Command vol_up", "Step 2 of Start") is built here.
import type { Config, ValidationError } from './model';

export interface Problem {
  /** where it is, in words: "Command vol_up" */
  label: string;
  msg: string;
  path: (string | number)[];
  /** a device command / input / power code the user can open to fix it */
  open?: { where: 'commands' | 'inputs' | 'power'; name: string };
}

const nth = (n: number | string) => (typeof n === 'number' ? n + 1 : n);

/** Where the error is, in words, relative to its device or activity. */
export function describePath(path: (string | number)[]): { label: string; open?: Problem['open'] } {
  const rest = path.slice(2);
  const [a, b, c] = rest;
  if (path[0] === 'devices') {
    if (a === 'commands' && b !== undefined) return { label: `Command ${String(b)}`, open: { where: 'commands', name: String(b) } };
    if (a === 'inputs' && b !== undefined) return { label: `Input ${String(b)}`, open: { where: 'inputs', name: String(b) } };
    if (a === 'power') return b !== undefined ? { label: `Power ${String(b)}`, open: { where: 'power', name: String(b) } } : { label: 'Power' };
    if (a === 'aliases') return { label: `Alias ${nth(b ?? '')}` };
    if (a === 'settle_ms') return { label: 'Settle time' };
    if (a === 'entity') return { label: 'Entity' };
    if (a === 'id') return { label: 'Id' };
    if (a === 'name') return { label: 'Name' };
    return { label: a === undefined ? 'Device' : String(a) };
  }
  if (path[0] === 'activities') {
    if (a === 'devices') return { label: `Device ${nth(b ?? '')}${c ? ` (${String(c)})` : ''}` };
    if (a === 'steps') return { label: `Start step ${nth(b ?? '')}` };
    if (a === 'off') return { label: `Stop step ${nth(b ?? '')}` };
    if (a === 'keys') return { label: `Key ${String(b)}` };
    if (a === 'id') return { label: 'Id' };
    if (a === 'name') return { label: 'Name' };
    return { label: a === undefined ? 'Activity' : String(a) };
  }
  if (path[0] === 'editor') return { label: 'Editor notes' };
  return { label: path.join(' / ') };
}

/** Errors under `devices[index]` or `activities[index]`. */
export function problemsFor(errors: ValidationError[], section: 'devices' | 'activities', index: number, _config?: Config): Problem[] {
  void _config;
  return errors
    .filter((e) => e.path[0] === section && e.path[1] === index)
    .map((e) => ({ ...describePath(e.path), msg: e.msg, path: e.path }));
}

/** Errors that no device, activity, key or page screen owns (top-level, `editor`, whole-config). */
export function orphanProblems(errors: ValidationError[]): Problem[] {
  return errors
    .filter((e) => !['devices', 'activities', 'keys', 'pages'].includes(String(e.path[0] ?? '')))
    .map((e) => ({ ...describePath(e.path), msg: e.msg, path: e.path }));
}
