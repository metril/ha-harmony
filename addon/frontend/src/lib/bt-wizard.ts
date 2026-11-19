// Pure state of the pairing wizard: from what the hub last reported and what we asked, what step the user is on.
// The views poll `bt/status` and call `derive`; nothing here touches the network or the DOM.
import type { BtState } from './api';

export interface Run {
  mode: 'host' | 'remote';
  /** addresses and the last pairing outcome the hub reported before we opened the window */
  before: { hosts: string[]; remotes: string[]; pairing: string };
  /** a remote chosen from the candidates */
  picked: string | null;
  /** the window was seen open at least once after we asked */
  seenOpen: boolean;
  startedAt: number;
}
export type Phase =
  | { t: 'opening' }
  | { t: 'waiting' }
  | { t: 'scanning'; candidates: NonNullable<BtState['candidates']> }
  | { t: 'bonding'; addr: string }
  | { t: 'paired'; addr: string; name?: string; transport?: 'classic' | 'le' }
  | { t: 'failed'; result: 'failed' | 'refused' | 'timeout' };

/** A window that was asked for but never shows in the status is given this long before we call it a timeout. */
export const OPEN_GRACE_MS = 8000;

export function startRun(mode: Run['mode'], state: BtState | null, now: number): Run {
  return {
    mode,
    before: { hosts: (state?.hosts ?? []).map((h) => h.addr), remotes: (state?.remotes ?? []).map((r) => r.addr), pairing: JSON.stringify(state?.pairing ?? null) },
    picked: null,
    seenOpen: false,
    startedAt: now,
  };
}
export const isOpen = (run: Run, s: BtState | null): boolean => !!s && (s.window === run.mode || (run.mode === 'host' && s.discoverable));

/** Record that the window has been seen open (call on every status). */
export function observe(run: Run, s: BtState | null): Run {
  return !run.seenOpen && isOpen(run, s) ? { ...run, seenOpen: true } : run;
}

export function derive(run: Run, s: BtState | null, now: number): Phase {
  const open = isOpen(run, s);
  const before = new Set(run.mode === 'host' ? run.before.hosts : run.before.remotes);
  const now_ = (run.mode === 'host' ? s?.hosts : s?.remotes) ?? [];
  const fresh = (run.seenOpen && !open) || JSON.stringify(s?.pairing ?? null) !== run.before.pairing;
  const p = s?.pairing && fresh && (!s.pairing.kind || s.pairing.kind === run.mode) ? s.pairing : null;
  const added = now_.find((e) => !before.has(e.addr) && (!p?.addr || e.addr === p.addr)) ?? now_.find((e) => !before.has(e.addr));
  if (added) return { t: 'paired', addr: added.addr, ...(added.name ? { name: added.name } : {}), ...('transport' in added ? { transport: added.transport } : {}) };
  if (p?.result === 'ok' && p.addr) return { t: 'paired', addr: p.addr, ...(p.name ? { name: p.name } : {}), ...(p.transport ? { transport: p.transport } : {}) };
  if (p && p.result !== 'ok') return { t: 'failed', result: p.result };
  if (open) {
    if (run.mode === 'host') return { t: 'waiting' };
    return run.picked ? { t: 'bonding', addr: run.picked } : { t: 'scanning', candidates: s?.candidates ?? [] };
  }
  if (run.picked && now - run.startedAt < OPEN_GRACE_MS * 2 && !run.seenOpen) return { t: 'bonding', addr: run.picked };
  if (run.seenOpen || now - run.startedAt > OPEN_GRACE_MS) return { t: 'failed', result: 'timeout' };
  return { t: 'opening' };
}

/** Sentences for a live region: what is happening now (no running numbers, so it is not read out every second). */
export function announce(ph: Phase, mode: Run['mode']): string {
  switch (ph.t) {
    case 'opening':
      return 'Opening the pairing window…';
    case 'waiting':
      return 'Pairing window open. Waiting for a device to pair.';
    case 'scanning':
      return ph.candidates.length ? `${ph.candidates.length} remote${ph.candidates.length === 1 ? '' : 's'} found. Choose yours.` : 'Looking for remotes in pairing mode…';
    case 'bonding':
      return 'Pairing with the remote…';
    case 'paired':
      return mode === 'host' ? 'Paired. Name the device and choose its commands.' : 'Paired. Choose what its buttons do.';
    case 'failed':
      return ph.result === 'timeout' ? 'Nothing paired before the window closed.' : 'Pairing did not finish.';
  }
}
