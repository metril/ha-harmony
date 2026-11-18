// State machine of the add-device wizard: source -> brand -> type -> power test -> model -> review.
// Pure: the view runs the API calls and feeds the results back as actions, so every path (including
// archive errors) is tested without a DOM.
import type { IrCandidate, IrDeviceResult, IrModel } from './api';

export type WStep = 'source' | 'brand' | 'type' | 'test' | 'model' | 'review' | 'paste' | 'entity';
export type WSource = 'archive' | 'paste' | 'ha';
export type Scope = 'brands' | 'candidates' | 'models' | 'device';
export interface WError {
  code: string;
  msg: string;
  scope: Scope;
}
export interface WState {
  step: WStep;
  source: WSource | null;
  brand: { name: string; slug: string } | null;
  type: string | null;
  candidates: IrCandidate[] | null;
  truncated: boolean;
  /** candidates ran out without a "yes" */
  exhausted: boolean;
  idx: number;
  tried: { idx: number; result: 'yes' | 'no' | 'skipped' }[];
  confirmed: IrCandidate | null;
  model: IrModel | null;
  result: IrDeviceResult | null;
  /** command names ticked on the review step */
  selected: string[];
  loading: boolean;
  error: WError | null;
}
export type WAction =
  | { t: 'source'; source: WSource }
  | { t: 'brand'; brand: { name: string; slug: string } }
  | { t: 'type'; type: string }
  | { t: 'candidates'; candidates: IrCandidate[]; truncated?: boolean }
  | { t: 'answer'; answer: 'yes' | 'no' | 'skip' }
  | { t: 'skipTest' }
  | { t: 'model'; model: IrModel }
  | { t: 'device'; result: IrDeviceResult }
  | { t: 'fail'; scope: Scope; code: string; msg: string }
  | { t: 'retry' }
  | { t: 'back' }
  | { t: 'toggle'; name: string }
  | { t: 'all'; on: boolean }
  | { t: 'draft'; result: IrDeviceResult }
  | { t: 'reset' };

export const initial = (): WState => ({
  step: 'source', source: null, brand: null, type: null, candidates: null, truncated: false, exhausted: false, idx: 0, tried: [],
  confirmed: null, model: null, result: null, selected: [], loading: false, error: null,
});

export const OFFLINE_CODES = ['archive_offline', 'archive_timeout'];
export const isOffline = (e: WError | null): boolean => !!e && OFFLINE_CODES.includes(e.code);

/** The dots of the progress row for a source. */
export function stepsFor(source: WSource | null): { id: WStep; label: string }[] {
  if (source === 'paste') return [{ id: 'source', label: 'Source' }, { id: 'paste', label: 'Codes' }, { id: 'review', label: 'Review' }];
  if (source === 'ha') return [{ id: 'source', label: 'Source' }, { id: 'entity', label: 'Entity' }, { id: 'review', label: 'Review' }];
  return [
    { id: 'source', label: 'Source' }, { id: 'brand', label: 'Brand' }, { id: 'type', label: 'Type' },
    { id: 'test', label: 'Power test' }, { id: 'model', label: 'Model' }, { id: 'review', label: 'Review' },
  ];
}
const BACK: Record<WStep, WStep> = { source: 'source', brand: 'source', type: 'brand', test: 'type', model: 'test', review: 'model', paste: 'source', entity: 'source' };

function afterCandidate(s: WState, idx: number): WState {
  if (s.candidates && idx >= s.candidates.length) return { ...s, idx, exhausted: true, step: 'model', confirmed: null };
  return { ...s, idx };
}
const names = (r: IrDeviceResult): string[] => Object.keys(r.device.commands ?? {});

export function reduce(s: WState, a: WAction): WState {
  switch (a.t) {
    case 'reset':
      return initial();
    case 'source':
      return { ...initial(), source: a.source, step: a.source === 'archive' ? 'brand' : a.source === 'paste' ? 'paste' : 'entity' };
    case 'brand':
      return { ...s, brand: a.brand, type: null, candidates: null, confirmed: null, exhausted: false, model: null, result: null, tried: [], idx: 0, error: null, step: 'type' };
    case 'type':
      return { ...s, type: a.type, candidates: null, truncated: false, exhausted: false, confirmed: null, tried: [], idx: 0, model: null, result: null, error: null, loading: true, step: 'test' };
    case 'candidates': {
      const base = { ...s, candidates: a.candidates, truncated: !!a.truncated, loading: false, error: null, idx: 0, tried: [] };
      // nothing to test: go to the model list and say why
      return a.candidates.length ? base : { ...base, step: 'model', exhausted: true };
    }
    case 'answer': {
      const c = s.candidates?.[s.idx];
      if (s.step !== 'test' || !c) return s;
      if (a.answer === 'yes') return { ...s, confirmed: c, tried: [...s.tried, { idx: s.idx, result: 'yes' }], step: 'model', exhausted: false };
      return afterCandidate({ ...s, tried: [...s.tried, { idx: s.idx, result: a.answer === 'no' ? 'no' : 'skipped' }] }, s.idx + 1);
    }
    case 'skipTest':
      return { ...s, step: 'model', confirmed: null, exhausted: false, loading: false, error: null };
    case 'model':
      return { ...s, model: a.model, result: null, loading: true, error: null };
    case 'device':
    case 'draft':
      return { ...s, result: a.result, selected: names(a.result), loading: false, error: null, step: 'review' };
    case 'fail': {
      const error: WError = { code: a.code, msg: a.msg, scope: a.scope };
      if (a.code === 'unknown_brand') return { ...s, error, loading: false, brand: null, step: 'brand' };
      // a codeset with too many commands cannot be imported: stay on the model list, pick another
      if (a.code === 'archive_too_large') return { ...s, error, loading: false, model: null, step: 'model' };
      // candidates that cannot load: the model list still works
      if (a.scope === 'candidates' && !OFFLINE_CODES.includes(a.code)) return { ...s, error, loading: false, step: 'model', exhausted: true };
      return { ...s, error, loading: false };
    }
    case 'retry':
      return { ...s, error: null, loading: true };
    case 'back': {
      if (s.step === 'source') return s;
      const step = BACK[s.step];
      // from the model list back past the test when nothing was tested
      return { ...s, step, error: null, loading: false, ...(step === 'test' && !s.candidates ? { step: 'type' as WStep } : {}), ...(s.step === 'review' ? { result: null, model: null } : {}) };
    }
    case 'toggle':
      return { ...s, selected: s.selected.includes(a.name) ? s.selected.filter((n) => n !== a.name) : [...s.selected, a.name] };
    case 'all':
      return { ...s, selected: a.on && s.result ? names(s.result) : [] };
  }
}

export const currentCandidate = (s: WState): IrCandidate | null => s.candidates?.[s.idx] ?? null;
/** "Code 2 of 7" */
export const candidateLabel = (s: WState): string => (s.candidates ? `Code ${Math.min(s.idx + 1, s.candidates.length)} of ${s.candidates.length}` : '');
export const progressIndex = (s: WState): number => Math.max(0, stepsFor(s.source).findIndex((x) => x.id === s.step));

/** The config device to add: the converted block with only the ticked commands. */
export function draftDevice(s: WState): IrDeviceResult['device'] | null {
  const d = s.result?.device;
  if (!d) return null;
  const commands = Object.fromEntries(Object.entries(d.commands ?? {}).filter(([n]) => s.selected.includes(n)));
  return { ...d, commands };
}
/** Models worth showing first: the names the confirmed candidate came from. */
export const likelyModels = (s: WState): string[] => s.confirmed?.models ?? [];
