// Turn server {path, msg} errors into a route, a field id and per-area counts.
import type { Config, ValidationError } from './model';

export interface Located {
  route: string; // rail section id
  field: string; // stable field id, usable as an element id / aria target
  msg: string;
  path: string;
}

const SECTIONS: Record<string, string> = {
  devices: 'devices',
  activities: 'activities',
  keys: 'keys',
  pages: 'pages',
};

export function fieldId(path: (string | number)[]): string {
  return 'f-' + path.map(String).join('.').replace(/[^A-Za-z0-9_.-]/g, '_');
}

export function locate(e: ValidationError, config?: Config | null): Located {
  const [head, second, third] = e.path;
  const route = typeof head === 'string' ? (SECTIONS[head] ?? 'overview') : 'overview';
  let r = route;
  if (route === 'devices' && typeof second === 'number') {
    const tab = third === 'power' ? 'power' : third === 'inputs' ? 'inputs' : third === 'commands' ? 'commands' : 'settings';
    r = `devices/${config?.devices?.[second]?.id ?? second}/${tab}`;
  }
  return { route: r, field: fieldId(e.path), msg: e.msg, path: e.path.join('.') };
}

export function summarizeErrors(errors: ValidationError[], config?: Config | null) {
  const located = errors.map((e) => locate(e, config));
  const bySection: Record<string, number> = {};
  const byField = new Map<string, string[]>();
  const unlocated: Located[] = [];
  for (const l of located) {
    const sec = l.route.split('/')[0] as string;
    bySection[sec] = (bySection[sec] ?? 0) + 1;
    if (!l.path || l.route === 'overview') unlocated.push(l);
    byField.set(l.field, [...(byField.get(l.field) ?? []), l.msg]);
  }
  return { located, bySection, byField, unlocated, total: errors.length, first: located[0] };
}
