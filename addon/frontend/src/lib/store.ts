// Draft store: working copy, ~600 ms debounced autosave through the single PUT queue,
// optimistic edits with rollback, validation + usage via preview, conflict handling.
import { Api, ApiError, DraftQueue, type Meta, type Preview, type Usage } from './api';
import { boundEditor } from './activity-edit';
import { diffConfig, type DiffRow } from './diff';
import { summarizeErrors } from './errors';
import { applyUsageLimits, type Config, type ValidationError } from './model';

export type SaveState = 'clean' | 'dirty' | 'saving' | 'saved' | 'error' | 'conflict';
export interface Toast {
  id: number;
  kind: 'info' | 'error';
  text: string;
  /** offer an Undo button (studio edits) */
  undo?: boolean;
}
type Applied = NonNullable<Meta['applied']> & { config?: Config };

export const clone = <T>(v: T): T => (v == null ? v : (JSON.parse(JSON.stringify(v)) as T));

export interface Stash {
  config: Config;
  /** the server rev the stashed edit was based on */
  rev: number | null;
  at: number;
}
const stashKey = (hub: string) => `hh-unsaved:${hub}`;

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
  /** client-side edit history: configs before each edit (Ctrl+Z), and the ones undone (Ctrl+Shift+Z) */
  private past: Config[] = [];
  private future: Config[] = [];
  private lastEditKey: { key: string; at: number } | null = null;
  private previewTimer: ReturnType<typeof setTimeout> | null = null;
  private queue: DraftQueue;
  private timer: ReturnType<typeof setTimeout> | null = null;
  /** the add-on could not be reached while saving; edits are kept and retried */
  offline = false;
  private retries = 0;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
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

  /** Unsaved edits from a moment ago, offered after a reload when the server never received them. */
  stashed: Stash | null = null;
  private store_(): Storage | null {
    try {
      return globalThis.sessionStorage ?? null;
    } catch {
      return null;
    }
  }
  private clearStash() {
    try {
      this.store_()?.removeItem(stashKey(this.hub));
    } catch { /* storage unavailable */ }
  }
  /** Page is going away (pagehide / tab hidden): send a pending draft save now and keep a copy in sessionStorage. */
  flushOnUnload(): void {
    if (this.saveState !== 'dirty' && this.saveState !== 'error' && this.saveState !== 'saving') return;
    const stash: Stash = { config: this.config, rev: this.queue.rev, at: Date.now() };
    try {
      this.store_()?.setItem(stashKey(this.hub), JSON.stringify(stash));
    } catch { /* quota or private mode: the keepalive save still goes */ }
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    void this.api.putDraftOnUnload(this.hub, this.config, this.queue.rev);
  }
  /** Read a stash left by the previous page; keep it only when the server's draft is older than it. */
  private readStash(serverRev: number | null) {
    this.stashed = null;
    try {
      const raw = this.store_()?.getItem(stashKey(this.hub));
      if (!raw) return;
      const s = JSON.parse(raw) as Stash;
      const sameAsServer = JSON.stringify(s.config) === JSON.stringify(this.config);
      if (sameAsServer || (s.rev != null && serverRev != null && serverRev > s.rev)) this.clearStash();
      else this.stashed = s;
    } catch {
      this.clearStash();
    }
  }
  restoreStash(): void {
    const s = this.stashed;
    if (!s) return;
    this.stashed = null;
    this.clearStash();
    this.replace(clone(s.config));
  }
  discardStash(): void {
    this.stashed = null;
    this.clearStash();
    this.emit();
  }

  private emit() {
    this.dispatchEvent(new Event('change'));
  }

  toast(text: string, kind: Toast['kind'] = 'info', undo = false) {
    const t: Toast = { id: ++this.toastId, kind, text, ...(undo ? { undo } : {}) };
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
      this.past = [];
      this.future = [];
      this.confirmedHasDraft = this.hasDraft;
      this.queue.rev = r.rev;
      this.savedAt = r.draft?.updated ?? null;
      this.saveState = 'clean';
      this.readStash(r.rev);
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
  edit(mutate: (c: Config) => void, coalesce?: string): void {
    const next = clone(this.config);
    mutate(next);
    this.replace(next, coalesce);
  }

  get canUndo(): boolean {
    return this.past.length > 0;
  }
  get canRedo(): boolean {
    return this.future.length > 0;
  }
  undo(): boolean {
    const prev = this.past.pop();
    if (!prev) return false;
    this.future.push(this.config);
    this.lastEditKey = null;
    this.apply_(prev);
    return true;
  }
  redo(): boolean {
    const next = this.future.pop();
    if (!next) return false;
    this.past.push(this.config);
    this.lastEditKey = null;
    this.apply_(next);
    return true;
  }

  /** Swap in a whole config as an optimistic edit. Edits sharing a `coalesce` key within 1.2 s are one undo step. */
  replace(next: Config, coalesce?: string): void {
    next = boundEditor(next);
    const now = Date.now();
    if (!(coalesce && this.lastEditKey?.key === coalesce && now - this.lastEditKey.at < 1200)) {
      this.past.push(this.config);
      if (this.past.length > 80) this.past.shift();
    }
    this.lastEditKey = coalesce ? { key: coalesce, at: now } : null;
    this.future = [];
    this.apply_(next);
  }

  private apply_(next: Config): void {
    this.config = next;
    this.hasDraft = true;
    if (this.saveState !== 'conflict') this.saveState = 'dirty';
    this.emit();
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.save(), this.debounceMs);
    if (this.retryTimer) clearTimeout(this.retryTimer);
    if (this.previewTimer) clearTimeout(this.previewTimer);
    this.previewTimer = setTimeout(() => void this.check(true), 150);
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
      if (this.saveState === 'saving') {
        this.saveState = 'saved';
        this.clearStash();
      }
      if (this.offline) {
        this.offline = false;
        this.toast('The add-on is back. Your edits are saved.');
      }
      this.retries = 0;
      this.emit();
      if (this.saveState === 'saved') await this.check();
    } catch (e) {
      if (e instanceof ApiError && e.isConflict) {
        this.saveState = 'conflict';
        this.emit();
        return;
      }
      if (e instanceof ApiError && (e.isNetwork || [502, 503, 504].includes(e.status))) {
        // the add-on is restarting or unreachable: keep the edits here and retry with a growing pause
        this.saveState = 'error';
        this.offline = true;
        if (this.retries === 0) this.toast('The add-on cannot be reached. Your edits are kept here and saved as soon as it is back.', 'error');
        this.emit();
        const wait = Math.min(30000, 2000 * 2 ** this.retries++);
        if (this.retryTimer) clearTimeout(this.retryTimer);
        this.retryTimer = setTimeout(() => void this.save(), wait);
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
  async check(live = false): Promise<void> {
    const seq = ++this.seq;
    try {
      // no saved draft: the add-on has nothing to default to (409 no_draft), so send the applied config;
      // a live check (right after an edit) sends the in-memory config so the canvas does not wait for the save
      const p = await this.api.preview(this.hub, live || !this.hasDraft ? clone(this.config) : undefined);
      if (seq !== this.seq) return;
      this.errors = p.errors;
      this.usage = p.usage;
      applyUsageLimits(p.usage);
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

  /** After a destructive server-side swap of the draft (discard, import, revert): reload, then keep what was replaced one Undo away. */
  private async swapWith(prev: Config, hadDraft: boolean, what: string, run: () => Promise<unknown>): Promise<void> {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    await run();
    await this.load();
    if (hadDraft || Object.keys(prev).length) {
      this.past = [clone(prev)];
      this.toast(what, 'info', true);
    }
  }

  async discard(): Promise<void> {
    await this.swapWith(clone(this.config), this.hasDraft, 'Draft discarded.', () => this.api.revert(this.hub, 'applied'));
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
    await this.swapWith(clone(this.config), this.hasDraft, 'Reverted to the previous version.', () => this.api.revert(this.hub, target));
  }

  async importYaml(text: string): Promise<void> {
    // the user already confirmed replacing the draft: only a live conflict check applies
    await this.swapWith(clone(this.config), this.hasDraft, 'Draft replaced by the imported file.', () =>
      this.api.importYaml(this.hub, text, this.saveState === 'conflict' ? null : this.queue.rev),
    );
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
