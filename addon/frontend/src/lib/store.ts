// Draft store: working copy, ~600 ms debounced autosave through the single PUT queue,
// optimistic edits with rollback, validation + usage via preview, conflict handling.
import { Api, ApiError, DraftQueue, type Meta, type Preview, type Usage } from './api';
import { diffConfig, type DiffRow } from './diff';
import { summarizeErrors } from './errors';
import type { Config, ValidationError } from './model';

export type SaveState = 'clean' | 'dirty' | 'saving' | 'saved' | 'error' | 'conflict';
export interface Toast {
  id: number;
  kind: 'info' | 'error';
  text: string;
}
type Applied = NonNullable<Meta['applied']> & { config?: Config };

export const clone = <T>(v: T): T => (v == null ? v : (JSON.parse(JSON.stringify(v)) as T));

export class Store extends EventTarget {
  loaded = false;
  loadError: string | null = null;
  config: Config = {};
  /** last config the server confirmed (rollback target) */
  confirmed: Config = {};
  confirmedHasDraft = false;
  hasDraft = false;
  applied: Applied | null = null;
  history: Meta['history'] = [];
  historyMax = 5;
  saveState: SaveState = 'clean';
  savedAt: number | null = null;
  errors: ValidationError[] = [];
  usage: Usage | null = null;
  hash: string | null = null;
  /** per-page preview results from the last check (error tolerant: good pages decode even when others fail) */
  pages: Preview['pages'] = [];
  missingPages: Preview['missing_pages'] = [];
  toasts: Toast[] = [];
  private queue: DraftQueue;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private seq = 0;
  private toastId = 0;

  constructor(
    public api: Api,
    public hub: string,
    private debounceMs = 600,
  ) {
    super();
    this.queue = new DraftQueue(api, hub);
  }

  private emit() {
    this.dispatchEvent(new Event('change'));
  }

  toast(text: string, kind: Toast['kind'] = 'info') {
    const t = { id: ++this.toastId, kind, text };
    this.toasts = [...this.toasts, t];
    this.emit();
    if (kind === 'info') setTimeout(() => this.dismissToast(t.id), 8000);
    return t.id;
  }
  dismissToast(id: number) {
    this.toasts = this.toasts.filter((t) => t.id !== id);
    this.emit();
  }

  get appliedConfig(): Config | null {
    return this.applied?.config ?? null;
  }
  get diff(): DiffRow[] {
    return this.hasDraft ? diffConfig(this.appliedConfig, this.config) : [];
  }
  get errorSummary() {
    return summarizeErrors(this.errors, this.config);
  }
  get isFirstRun(): boolean {
    return this.loaded && !this.hasDraft && !this.applied;
  }

  async load(): Promise<void> {
    this.loadError = null;
    try {
      const r = await this.api.getConfig(this.hub, true);
      this.applied = r.applied;
      this.history = r.history;
      this.historyMax = r.history_max;
      this.hasDraft = r.draft !== null;
      this.config = clone(r.draft?.config ?? r.applied?.config ?? {});
      this.confirmed = clone(this.config);
      this.confirmedHasDraft = this.hasDraft;
      this.queue.rev = r.rev;
      this.savedAt = r.draft?.updated ?? null;
      this.saveState = 'clean';
      this.loaded = true;
      this.emit();
      if (this.hasDraft || this.applied) await this.check();
      else {
        this.errors = [];
        this.usage = null;
        this.hash = null;
        this.emit();
      }
    } catch (e) {
      this.loadError = e instanceof Error ? e.message : String(e);
      this.loaded = true;
      this.emit();
    }
  }

  /** Optimistic edit: mutate a copy, show it now, save after the debounce. */
  edit(mutate: (c: Config) => void): void {
    const next = clone(this.config);
    mutate(next);
    this.replace(next);
  }

  /** Swap in a whole config as an optimistic edit. */
  replace(next: Config): void {
    this.config = next;
    this.hasDraft = true;
    if (this.saveState !== 'conflict') this.saveState = 'dirty';
    this.emit();
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.save(), this.debounceMs);
  }

  /** Save now (used by Apply and tests). */
  async flush(): Promise<void> {
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    if (this.saveState === 'dirty') await this.save();
  }

  private async save(force = false): Promise<void> {
    this.timer = null;
    if (this.saveState === 'conflict' && !force) return;
    const sent = clone(this.config);
    this.saveState = 'saving';
    this.emit();
    try {
      const r = await this.queue.enqueue(sent, { force });
      this.history = r.history;
      this.applied = this.applied && r.applied ? { ...this.applied, ...r.applied } : this.applied;
      this.confirmed = sent;
      this.confirmedHasDraft = true;
      this.savedAt = r.draft_updated;
      if (this.saveState === 'saving') this.saveState = 'saved';
      this.emit();
      if (this.saveState === 'saved') await this.check();
    } catch (e) {
      if (e instanceof ApiError && e.isConflict) {
        this.saveState = 'conflict';
        this.emit();
        return;
      }
      // roll back to what the server holds, and say so
      this.config = clone(this.confirmed);
      this.hasDraft = this.confirmedHasDraft;
      this.saveState = 'error';
      this.toast(`Could not save your change, so it was undone. ${e instanceof Error ? e.message : ''}`.trim(), 'error');
    }
  }

  /** Preview the saved draft (or the applied config): errors, per-page results and usage meters. */
  async check(): Promise<void> {
    const seq = ++this.seq;
    try {
      // no saved draft: the add-on has nothing to default to (409 no_draft), so send the applied config
      const p = await this.api.preview(this.hub, this.hasDraft ? undefined : clone(this.config));
      if (seq !== this.seq) return;
      this.errors = p.errors;
      this.usage = p.usage;
      this.hash = p.hash;
      this.pages = p.pages;
      this.missingPages = p.missing_pages;
    } catch {
      if (seq !== this.seq) return; // network trouble: keep what we showed before
    }
    this.emit();
  }

  /** Conflict banner: take the server's copy. */
  async reloadFromServer(): Promise<void> {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    await this.load();
  }
  /** Conflict banner: keep mine and write it over the other tab's draft. */
  async overwrite(): Promise<void> {
    this.saveState = 'dirty';
    await this.save(true);
  }

  async discard(): Promise<void> {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    await this.api.revert(this.hub, 'applied');
    await this.load();
  }

  /** Saves pending edits, then applies the saved draft. Throws ApiError (422 carries errors). */
  async applyDraft(): Promise<number> {
    await this.flush();
    if (this.saveState === 'conflict' || this.saveState === 'error') throw new ApiError(409, 'conflict', 'Resolve the save problem first.');
    try {
      const m = await this.api.apply(this.hub);
      if (!m.applied) throw new ApiError(500, 'bad_response', 'The add-on did not confirm the apply.');
      this.applied = { ...m.applied, config: clone(this.config) };
      this.history = m.history;
      this.queue.rev = m.rev;
      this.hasDraft = false;
      this.confirmed = clone(this.config);
      this.confirmedHasDraft = false;
      this.saveState = 'clean';
      this.errors = [];
      this.emit();
      return m.applied.rev;
    } catch (e) {
      if (e instanceof ApiError && e.status === 422) {
        this.errors = e.errors;
        this.emit();
      }
      throw e;
    }
  }

  async revertTo(target: 'previous'): Promise<void> {
    await this.api.revert(this.hub, target);
    await this.load();
  }

  async importYaml(text: string): Promise<void> {
    // the user already confirmed replacing the draft: only a live conflict check applies
    await this.api.importYaml(this.hub, text, this.saveState === 'conflict' ? null : this.queue.rev);
    await this.load();
  }

  /** Restore an applied version as the draft: fetch its source config, then replace the draft with it. */
  async restoreAsDraft(rev: number): Promise<void> {
    const h = await this.api.historyEntry(this.hub, rev);
    this.replace(clone(h.config));
    await this.flush();
  }
}

let current: Store | null = null;
export const setStore = (s: Store) => (current = s);
export const getStore = (): Store => {
  if (!current) throw new Error('store not initialised');
  return current;
};
