// What the draft asks of Home Assistant: entities it reads or acts on, and the services it calls.
import type { Config } from './model';

export interface Permissions {
  entities: { id: string; uses: string[] }[];
  services: { service: string; uses: number }[];
}
type Rec = Record<string, unknown>;

function haOf(a: unknown): { service: string; ids: string[] } | null {
  const h = a && typeof a === 'object' ? ((a as Rec)['ha'] as { service?: string; target?: { entity_id?: string | string[] } } | undefined) : undefined;
  if (!h?.service) return null;
  const e = h.target?.entity_id;
  return { service: h.service, ids: Array.isArray(e) ? e : e ? [e] : [] };
}

export function permissions(cfg: Config): Permissions {
  const ents = new Map<string, Set<string>>();
  const svcs = new Map<string, number>();
  const ent = (id: string, use: string) => ents.set(id, (ents.get(id) ?? new Set()).add(use));
  const act = (a: unknown, use: string) => {
    const h = haOf(a);
    if (!h) return;
    svcs.set(h.service, (svcs.get(h.service) ?? 0) + 1);
    h.ids.forEach((i) => ent(i, use));
  };
  for (const d of cfg.devices ?? []) {
    const dn = d.name ?? d.id;
    if (d.entity) ent(d.entity, `device ${dn}`);
    for (const a of Object.values(d.power ?? {})) act(a, `device ${dn}`);
    for (const a of Object.values(d.inputs ?? {})) act(a, `device ${dn}`);
    for (const a of Object.values(d.commands ?? {})) act(a, `device ${dn}`);
  }
  for (const [k, a] of Object.entries(cfg.keys ?? {})) act(a, `key ${k}`);
  for (const a of cfg.activities ?? []) {
    const n = a.name ?? a.id;
    for (const [k, v] of Object.entries(a.keys ?? {})) act(v, `${n}: key ${k}`);
    for (const s of [...(a.steps ?? []), ...(a.off ?? [])]) act(s, `activity ${n}`);
  }
  for (const p of cfg.pages ?? [])
    for (const w of p.widgets) {
      const use = `page ${p.title || p.id}`;
      if (typeof w.entity === 'string') ent(w.entity, `${use}, ${w.type}`);
      act(w['action'], use);
      act(w['tap'], use);
    }
  return {
    entities: [...ents].map(([id, u]) => ({ id, uses: [...u] })).sort((a, b) => a.id.localeCompare(b.id)),
    services: [...svcs].map(([service, uses]) => ({ service, uses })).sort((a, b) => a.service.localeCompare(b.service)),
  };
}

/** Default tap of a tile on this entity (docs/ha-config.md): what "automatic" does. */
export function tileDefaultTap(entityId: string): string | null {
  const d = entityId.split('.')[0] ?? '';
  if (['light', 'switch', 'fan', 'cover', 'input_boolean', 'media_player', 'remote'].includes(d)) return `${d}.toggle`;
  if (['scene', 'script'].includes(d)) return `${d}.turn_on`;
  if (['button', 'input_button'].includes(d)) return `${d}.press`;
  return null;
}
