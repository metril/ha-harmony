// IR code helpers shared by the (future) code editor: byte counter, per-protocol ranges, summaries.
import type { IrAction } from './model';

const enc = new TextEncoder();
export const utf8Bytes = (s: string): number => enc.encode(s).length;

export const NAME_MAX = 31;
/** Remaining bytes (negative when over) plus a copy line that says what to change. */
export function byteCounter(s: string, max = NAME_MAX): { used: number; max: number; over: number; hint: string } {
  const used = utf8Bytes(s);
  const over = Math.max(0, used - max);
  return { used, max, over, hint: over ? `Shorten by ${over} byte${over === 1 ? '' : 's'}` : '' };
}

// From harmony_core/ir/model.py: max (addr, cmd).
export const PROTOCOLS: Record<string, { addr: number; cmd: number }> = {
  nec: { addr: 0xff, cmd: 0xff },
  necext: { addr: 0xffff, cmd: 0xffff },
  samsung32: { addr: 0xff, cmd: 0xff },
  rc5: { addr: 0x1f, cmd: 0x7f },
  rc6: { addr: 0xff, cmd: 0xff },
  sirc12: { addr: 0x1f, cmd: 0x7f },
  sirc15: { addr: 0xff, cmd: 0x7f },
  sirc20: { addr: 0x1fff, cmd: 0x7f },
};

export type CodeKind = 'proto' | 'pronto' | 'raw' | 'flipper' | 'smartir';
export const ENABLED_KINDS: CodeKind[] = ['proto', 'pronto', 'raw'];

export function kindOf(a: IrAction | undefined): CodeKind | null {
  if (!a) return null;
  if (a.proto !== undefined || a.addr !== undefined || a.cmd !== undefined) return 'proto';
  if (a.pronto !== undefined) return 'pronto';
  if (a.raw !== undefined) return 'raw';
  if (a.flipper !== undefined) return 'flipper';
  if (a.smartir !== undefined) return 'smartir';
  return null;
}

export interface CodeCheck {
  ok: boolean;
  /** field -> message, fields are proto/addr/cmd/pronto/hz/us/port_mask */
  errors: Record<string, string>;
}

const isInt = (v: unknown): v is number => typeof v === 'number' && Number.isInteger(v);

export function validateCode(a: IrAction): CodeCheck {
  const errors: Record<string, string> = {};
  const kind = kindOf(a);
  if (kind === 'proto') {
    const lim = a.proto ? PROTOCOLS[a.proto] : undefined;
    if (!lim) errors.proto = 'Pick a protocol';
    if (!isInt(a.addr) || a.addr < 0) errors.addr = 'Enter a whole number';
    else if (lim && a.addr > lim.addr) errors.addr = `${a.proto} address must be 0..${lim.addr}`;
    if (!isInt(a.cmd) || a.cmd < 0) errors.cmd = 'Enter a whole number';
    else if (lim && a.cmd > lim.cmd) errors.cmd = `${a.proto} command must be 0..${lim.cmd}`;
  } else if (kind === 'pronto') {
    const words = (a.pronto ?? '').trim().split(/\s+/).filter(Boolean);
    if (words.length < 6) errors.pronto = 'Pronto code is too short';
    else if (!words.every((w) => /^[0-9a-fA-F]{4}$/.test(w))) errors.pronto = 'Words must be 4 hex digits';
    else if (parseInt(words[0] as string, 16) !== 0) errors.pronto = 'Only 0000 (learned) Pronto codes are supported';
    else {
      const once = parseInt(words[2] as string, 16);
      const rep = parseInt(words[3] as string, 16);
      if (once + rep === 0) errors.pronto = 'Pronto code has no burst pairs';
      else if (words.length !== 4 + 2 * (once + rep))
        errors.pronto = `Pronto header says ${once}+${rep} pairs but there are ${words.length - 4} data words`;
    }
  } else if (kind === 'raw') {
    const r = a.raw as { hz?: number; us?: number[] };
    if (!isInt(r.hz) || r.hz < 1000 || r.hz > 100000) errors.hz = 'Carrier must be 1000..100000 Hz';
    if (!Array.isArray(r.us) || r.us.length < 2) errors.us = 'Enter at least one mark and one space';
    else if (!r.us.every((n) => isInt(n) && n > 0)) errors.us = 'Durations must be positive whole microseconds';
    else if (r.us.some((n, i) => i % 2 === 0 && n > 65535)) errors.us = 'A mark may not exceed 65535 us';
  } else {
    errors.kind = 'This code kind cannot be edited here yet';
  }
  if (a.port_mask !== undefined && (!isInt(a.port_mask) || a.port_mask < 1 || a.port_mask > 255))
    errors.port_mask = 'IR ports must be 1..255';
  return { ok: Object.keys(errors).length === 0, errors };
}

export function summarize(a: IrAction | undefined): string {
  switch (kindOf(a)) {
    case 'proto':
      return `${a?.proto ?? '?'} ${a?.addr ?? '?'}/${a?.cmd ?? '?'}`;
    case 'pronto':
      return `pronto, ${(a?.pronto ?? '').trim().split(/\s+/).length} words`;
    case 'raw':
      return `raw, ${a?.raw?.us?.length ?? 0} pulses @ ${a?.raw?.hz ?? '?'} Hz`;
    case 'flipper':
      return `flipper ${a?.flipper?.file ?? ''}`;
    case 'smartir':
      return `smartir ${a?.smartir?.command ?? ''}`;
    default:
      return 'no code';
  }
}

/** `s` cut to at most `max` UTF-8 bytes, never in the middle of a character. */
export function trimBytes(s: string, max = NAME_MAX): string {
  let out = '';
  let n = 0;
  for (const ch of s) {
    const b = utf8Bytes(ch);
    if (n + b > max) break;
    out += ch;
    n += b;
  }
  return out;
}
