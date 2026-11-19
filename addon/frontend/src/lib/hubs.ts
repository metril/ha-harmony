// Which hub id the editor works on, and whether Home Assistant is polling for it.
import type { ApplyStatus, HubsResponse } from './api';

export interface HubChoice {
  hub: string;
  /** why this id: an explicit ?hub=, the one linked hub, the newest of several, a stored one, or the local `default` */
  reason: 'explicit' | 'linked' | 'newest-linked' | 'remembered' | 'stored' | 'default';
  /** every id worth offering in a switcher (linked ones first) */
  choices: string[];
  /** true when at least one integration has polled the add-on */
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

/** The integration long-polls (about 55 s), so anything older than this is not polling. */
export const POLL_FRESH_S = 150;

export type LinkInfo =
  | { kind: 'unknown' }
  | { kind: 'none' } // no integration has ever polled this add-on
  | { kind: 'other'; ids: string[] } // integrations poll, but not for this hub id
  | { kind: 'stale'; ids: string[]; age: number } // polled for this id once, not lately
  | { kind: 'polling'; ids: string[] };

export function linkInfo(s: ApplyStatus | null, hub: string): LinkInfo {
  if (!s || !s.linked_hubs || s.now === undefined) return { kind: 'unknown' };
  const ids = s.linked_hubs.map((l) => l.id);
  if (!ids.length) return { kind: 'none' };
  const mine = s.linked_hubs.find((l) => l.id === hub);
  if (!mine) return { kind: 'other', ids };
  const age = s.now - mine.last_seen;
  return age > POLL_FRESH_S ? { kind: 'stale', ids, age } : { kind: 'polling', ids };
}

/** One sentence for a link problem, or null when Home Assistant is polling for this hub (or it is unknown). */
export function linkProblem(l: LinkInfo, hub: string): string | null {
  switch (l.kind) {
    case 'none':
      return 'No Home Assistant integration is linked to this add-on yet. You can keep working on a local draft.';
    case 'other':
      return `Home Assistant is not polling for this hub (${hub}). It polls for: ${l.ids.join(', ')}.`;
    case 'stale':
      return `Home Assistant is not polling for this hub (${hub}); last seen ${Math.round(l.age / 60)} min ago.`;
    default:
      return null;
  }
}
