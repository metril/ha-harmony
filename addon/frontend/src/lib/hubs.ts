// Which hub id the editor works on, and whether Home Assistant is connected for it.
import type { ApplyStatus, ConfigSummary, HubsResponse } from './api';

export interface HubChoice {
  hub: string;
  /** why this id: an explicit ?hub=, the one linked hub, the newest of several, a stored one, or the local `default` */
  reason: 'explicit' | 'linked' | 'newest-linked' | 'remembered' | 'stored' | 'default';
  /** every id worth offering in a switcher (linked ones first) */
  choices: string[];
  /** true when at least one integration has linked to the add-on */
  anyLinked: boolean;
}

const KEY = 'hh-hub';
export function rememberedHub(): string | null {
  try {
    return localStorage.getItem(KEY);
  } catch {
    return null;
  }
}
export function rememberHub(hub: string): void {
  try {
    localStorage.setItem(KEY, hub);
  } catch {
    /* storage unavailable */
  }
}

/** Explicit ?hub= wins; else the linked hub id (the newest when several); else a stored one; else `default`. */
export function pickHub(search: string, resp: HubsResponse | null, remembered: string | null = null): HubChoice {
  const explicit = new URLSearchParams(search).get('hub');
  const valid = explicit !== null && /^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$/.test(explicit);
  const linked = (resp?.linked ?? []).map((l) => l.id).filter((id) => id !== 'default' || (resp?.linked.length ?? 0) === 1);
  const stored = (resp?.hubs ?? []).map((h) => h.id);
  const choices = [...new Set([...linked, ...stored])];
  const anyLinked = (resp?.linked.length ?? 0) > 0;
  if (valid) return { hub: explicit, reason: 'explicit', choices, anyLinked };
  const first = remembered !== null && linked.includes(remembered) ? remembered : linked[0]; // else newest first
  if (first !== undefined) return { hub: first, reason: linked.length === 1 ? 'linked' : first === linked[0] ? 'newest-linked' : 'remembered', choices, anyLinked };
  if (stored.length && !stored.includes('default')) return { hub: stored[0] as string, reason: 'stored', choices, anyLinked };
  return { hub: 'default', reason: 'default', choices, anyLinked };
}

/** The display name an integration reported for `hub`, or '' (the id is then the only label). */
export function hubName(resp: HubsResponse | null, hub: string): string {
  return resp?.linked.find((l) => l.id === hub)?.name ?? '';
}
/** "Name (id)" for a switcher option; just the id when the hub has no name. */
export function hubLabel(resp: HubsResponse | null, hub: string): string {
  const n = hubName(resp, hub);
  return n ? `${n} (${hub})` : hub;
}

export type LinkInfo =
  | { kind: 'unknown' }
  | { kind: 'none' } // no integration has ever connected to this add-on
  | { kind: 'other'; ids: string[] } // integrations are connected, but not for this hub id
  | { kind: 'offline'; ids: string[] } // this hub id was linked once, its channel is closed now
  | { kind: 'connected'; ids: string[] };

/** From the `hubs` topic (who has linked) and the `hub:<id>` topic (`connected`). */
export function linkInfo(hubs: HubsResponse | null, s: ApplyStatus | null, hub: string): LinkInfo {
  if (!hubs || !s) return { kind: 'unknown' };
  const ids = hubs.linked.map((l) => l.id);
  if (s.connected) return { kind: 'connected', ids };
  if (!ids.length) return { kind: 'none' };
  return ids.includes(hub) ? { kind: 'offline', ids } : { kind: 'other', ids };
}

/** One sentence for a link problem, or null when Home Assistant is connected for this hub (or it is unknown). */
export function linkProblem(l: LinkInfo, hub: string): string | null {
  switch (l.kind) {
    case 'none':
      return 'No Home Assistant integration is linked to this add-on yet. You can keep working on a local draft.';
    case 'other':
      return `Home Assistant is not connected for this hub (${hub}). It is linked for: ${l.ids.join(', ')}.`;
    case 'offline':
      return `Home Assistant is not connected for this hub (${hub}) right now.`;
    default:
      return null;
  }
}

const CFG_ID = /^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$/;
/** Which library config to open: an explicit ?config=, else the one the hub runs, else the most recently updated; null = the library is empty. */
export function pickConfig(search: string, configs: ConfigSummary[], hubs: HubsResponse | null, hub: string): string | null {
  const explicit = new URLSearchParams(search).get('config');
  if (explicit !== null && CFG_ID.test(explicit) && configs.some((c) => c.id === explicit)) return explicit;
  const running = hubs?.hubs.find((h) => h.id === hub)?.config_id;
  if (running && configs.some((c) => c.id === running)) return running;
  return [...configs].sort((a, b) => b.updated - a.updated)[0]?.id ?? null;
}
/** "Config Name Rev N" for a hub row, or '' when the hub runs nothing. */
export function runningLabel(hubs: HubsResponse | null, hub: string): string {
  const h = hubs?.hubs.find((x) => x.id === hub);
  return h?.has_applied && h.config_name ? `${h.config_name} Rev ${h.rev ?? '?'}` : '';
}
