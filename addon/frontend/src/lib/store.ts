// Draft store: working copy, ~600 ms debounced autosave through the single PUT queue,
// optimistic edits with rollback, validation + usage via preview, conflict handling.
import { Api, ApiError, DraftQueue, type ConfigDetail, type HubState, type Preview, type RevInfo, type Usage } from './api';
import { boundEditor } from './activity-edit';
import { primeBtDefaults } from './bt';
import { diffConfig, type DiffRow } from './diff';
import { summarizeErrors } from './errors';
import { issueText, type Issue } from './issues';
import type { StudioCheck } from './studio-model';
import { applyUsageLimits, type Config, type ValidationError } from './model';

export type SaveState = 'clean' | 'dirty' | 'saving' | 'saved' | 'error' | 'conflict';
export interface Toast {
  id: number;
  kind: 'info' | 'error';
  text: string;
  /** offer an Undo button (studio edits) */
  undo?: boolean;
}

export const clone = <T>(v: T): T => (v == null ? v : (JSON.parse(JSON.stringify(v)) as T));

export interface Stash {
  config: Config;
  /** the server rev the stashed edit was based on */
  rev: number | null;
  at: number;
}
const stashKey = (configId: string) => `hh-unsaved:${configId}`;

export class Store extends EventTarget {
  loaded = false;
  loadError: string | null = null;
  config: Config = {};
  /** last config the server confirmed (rollback target) */
  confirmed: Config = {};
  confirmedHasDraft = false;
  hasDraft = false;
  /** the library config being edited, its name, and the revision the draft started from */
  configName = '';
  baseRev: number | null = null;
  /** revisions of the config, oldest first */
  revs: RevInfo[] = [];
  /** the config of revision `baseRev`: what "changes" are measured against */
  baseline: Config | null = null;
  /** the selected hub: what it runs and the versions it can revert to */
  hubState: HubState | null = null;
  saveState: SaveState = 'clean';
  savedAt: number | null = null;
  errors: ValidationError[] = [];
  /** non-blocking warnings of the last check (the add-on sends them among `errors`, marked `severity: warning`) */
  warnings: ValidationError[] = [];
  /** the Studio's own checks (label too wide, no glyph, row with no action); published by the open Studio */
  checks: StudioCheck[] = [];
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
    public configId: string,
    private debounceMs = 600,
  ) {
    super();
    this.queue = new DraftQueue(api, configId);
  }

  /** The open config was deleted elsewhere: autosave is off and the edits stay here. */
  deleted = false;
  /** This tab is deleting the open config itself (no "deleted" banner). */
  leaving = false;
  /** bumped on every config switch: late replies of the old config are dropped */
  private gen = 0;

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
      this.store_()?.removeItem(stashKey(this.configId));
    } catch { /* storage unavailable */ }
  }
  private writeStash(): void {
    const stash: Stash = { config: this.config, rev: this.queue.rev, at: Date.now() };
    try {
      this.store_()?.setItem(stashKey(this.configId), JSON.stringify(stash));
    } catch { /* quota or private mode: the keepalive save still goes */ }
  }
  /** Page is going away (pagehide / tab hidden): send a pending draft save now and keep a copy in sessionStorage. */
  flushOnUnload(): void {
    if (this.saveState !== 'dirty' && this.saveState !== 'error' && this.saveState !== 'saving') return;
    this.writeStash();
    if (this.deleted) return;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    void this.api.putDraftOnUnload(this.configId, this.config, this.queue.rev);
  }
  /** Read a stash left by the previous page; keep it only when the server's draft is older than it. */
  private readStash(serverRev: number | null) {
    this.stashed = null;
    try {
      const raw = this.store_()?.getItem(stashKey(this.configId));
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

  /** Publish the Studio's checks; notifies only when their text changed. */
  setChecks(next: StudioCheck[]): void {
    if (next.length === this.checks.length && next.every((c, i) => issueText(c) === issueText(this.checks[i] as StudioCheck))) return;
    this.checks = next;
    this.emit();
  }
  /** Take the user to an issue: a route, or a Studio widget (the Studio listens for `hh-show-issue`). */
  showIssue(i: Issue): void {
    const t = i.target;
    if (!t) return;
    if (t.kind === 'route') {
      location.hash = '#/' + t.route;
      requestAnimationFrame(() => document.querySelector('hh-app')?.shadowRoot?.querySelector('main')?.focus());
      return;
    }
    location.hash = '#/remote';
    this.dispatchEvent(new CustomEvent('hh-show-issue', { detail: i }));
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

  get applied(): HubState['applied'] {
    return this.hubState?.applied ?? null;
  }
  get history(): HubState['history'] {
    return this.hubState?.history ?? [];
  }
  get historyMax(): number {
    return this.hubState?.history_max ?? 5;
  }
  /** the config of the revision the draft is based on (null: never saved as a revision) */
  get appliedConfig(): Config | null {
    return this.baseline;
  }
  /** the draft differs from its base revision (or has none) */
  get dirty(): boolean {
    if (!this.hasDraft) return false;
    return this.baseRev === null || !this.baseline || JSON.stringify(this.baseline) !== JSON.stringify(this.config);
  }
  get diff(): DiffRow[] {
    return this.hasDraft ? diffConfig(this.baseline, this.config) : [];
  }
  get errorSummary() {
    return summarizeErrors(this.errors, this.config);
  }
  get isFirstRun(): boolean {
    return this.loaded && !this.hasDraft && this.revs.length === 0;
  }

  /** Take a config response's revision data (summary, base revision's config). */
  private async takeDetail(r: ConfigDetail): Promise<void> {
    this.configName = r.name;
    this.revs = r.revs;
    this.baseRev = r.base_rev;
    this.baseline = r.base_rev !== null ? clone((await this.api.getRev(this.configId, r.base_rev)).config) : null;
  }
  async refreshHub(): Promise<void> {
    try {
      this.hubState = await this.api.hubState(this.hub);
    } catch {
      this.hubState = null;
    }
    this.emit();
  }
  /** Reload the revision list and base (another tab saved a revision, or the library changed). */
  async refreshRevs(): Promise<void> {
    try {
      await this.takeDetail(await this.api.getConfig(this.configId));
      this.emit();
    } catch { /* keep what is shown */ }
  }

  async load(): Promise<void> {
    this.loadError = null;
    try {
      const [r] = await Promise.all([this.api.getConfig(this.configId), this.refreshHub()]);
      await this.takeDetail(r);
      this.hasDraft = r.draft !== null;
      this.config = clone(r.draft?.config ?? {});
      this.confirmed = clone(this.config);
      this.past = [];
      this.future = [];
      this.confirmedHasDraft = this.hasDraft;
      this.queue.rev = r.draft_rev;
      this.savedAt = r.draft?.updated ?? null;
      this.saveState = 'clean';
      this.readStash(r.draft_rev);
      this.loaded = true;
      this.emit();
      void primeBtDefaults(this.api, this.config, () => this.emit());
      if (this.hasDraft) await this.check();
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
    void primeBtDefaults(this.api, next, () => this.emit()); // a pasted or restored config may bring a Bluetooth device on the default set
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.save(), this.debounceMs);
    if (this.retryTimer) clearTimeout(this.retryTimer);
    if (this.previewTimer) clearTimeout(this.previewTimer);
    this.previewTimer = setTimeout(() => void this.check(true), 150);
  }

  /** Save now (used by Apply and tests). */
  async flush(): Promise<void> {
    if (this.deleted) return;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    if (this.saveState === 'dirty') await this.save();
  }

  private async save(force = false): Promise<void> {
    this.timer = null;
    if (this.deleted) return;
    if (this.saveState === 'conflict' && !force) return;
    const gen = this.gen;
    const sent = clone(this.config);
    this.saveState = 'saving';
    this.emit();
    try {
      const r = await this.queue.enqueue(sent, { force });
      if (gen !== this.gen) return;
      this.baseRev = r.base_rev;
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
      if (gen !== this.gen) return;
      if (e instanceof ApiError && e.status === 404 && e.code === 'not_found') {
        void this.checkDeleted();
        return;
      }
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
      void live; // the preview is stateless: it always checks the in-memory config
      const p = await this.api.preview(this.hub, clone(this.config));
      if (seq !== this.seq) return;
      this.errors = p.errors.filter((e) => e.severity !== 'warning');
      this.warnings = p.errors.filter((e) => e.severity === 'warning');
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

  /** Drop the unsaved changes: back to the base revision. */
  async discard(): Promise<void> {
    if (this.baseRev === null) return;
    await this.restoreRev(this.baseRev, 'Changes discarded.');
  }

  /** Save the draft as the next numbered revision. */
  async saveRevision(note = ''): Promise<number> {
    await this.flush();
    if (this.saveState === 'conflict' || this.saveState === 'error') throw new ApiError(409, 'conflict', 'Resolve the save problem first.');
    if (!this.hasDraft) {
      // a blank config has no draft yet: store the (empty) editing state first
      await this.api.putDraft(this.configId, this.config, this.queue.rev).then((r) => (this.queue.rev = r.draft_rev));
      this.hasDraft = true;
    }
    const r = await this.api.saveRev(this.configId, note);
    this.queue.rev = r.draft_rev;
    this.baseRev = r.rev.rev;
    this.baseline = clone(this.config);
    this.revs = [...this.revs, r.rev];
    this.confirmed = clone(this.config);
    this.confirmedHasDraft = true;
    this.emit();
    return r.rev.rev;
  }

  private recordApplied(m: HubState): void {
    if (m.hub === this.hub) this.hubState = m;
  }
  /** Saves pending edits, then applies the draft (as a new revision, or the newest when unchanged) to `hub`. Throws ApiError (422 carries errors). */
  async applyDraft(hub = this.hub): Promise<number> {
    await this.flush();
    if (this.saveState === 'conflict' || this.saveState === 'error') throw new ApiError(409, 'conflict', 'Resolve the save problem first.');
    try {
      const m = await this.api.applyToHub(hub, this.configId);
      if (!m.applied) throw new ApiError(500, 'bad_response', 'The add-on did not confirm the apply.');
      this.recordApplied(m);
      await this.refreshRevs();
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
  /** Apply a saved revision of this config to `hub`. */
  async applyRev(hub: string, rev: number): Promise<number> {
    const m = await this.api.applyToHub(hub, this.configId, rev);
    if (!m.applied) throw new ApiError(500, 'bad_response', 'The add-on did not confirm the apply.');
    this.recordApplied(m);
    this.emit();
    return m.applied.rev;
  }

  async revertTo(hub = this.hub): Promise<HubState> {
    const m = await this.api.revertHub(hub);
    this.recordApplied(m);
    this.emit();
    return m;
  }

  async importYaml(text: string): Promise<void> {
    // the user already confirmed replacing the draft: only a live conflict check applies
    await this.swapWith(clone(this.config), this.hasDraft, 'Draft replaced by the imported file.', () =>
      this.api.importYaml(this.configId, text, this.saveState === 'conflict' ? null : this.queue.rev),
    );
  }

  /** Copy a revision into the draft (the old draft stays one Undo away). */
  async restoreRev(rev: number, what = `Rev ${rev} restored to the draft.`): Promise<void> {
    await this.swapWith(clone(this.config), this.hasDraft, what, () => this.api.restoreRev(this.configId, rev));
  }

  /** Switch to another library config in place: stash/save the old one's edits, then reset every per-config field and load the new one. */
  async switchTo(id: string): Promise<void> {
    if (id === this.configId) return;
    try {
      await this.flush();
    } catch { /* the stash below keeps the edits */ }
    if (!this.deleted && ['dirty', 'error', 'saving', 'conflict'].includes(this.saveState)) this.writeStash();
    for (const t of [this.timer, this.retryTimer, this.previewTimer]) if (t) clearTimeout(t);
    this.timer = this.retryTimer = this.previewTimer = null;
    this.gen++;
    this.seq++;
    this.configId = id;
    this.queue = new DraftQueue(this.api, id);
    this.deleted = false;
    this.leaving = false;
    this.loaded = false;
    this.loadError = null;
    this.config = {};
    this.confirmed = {};
    this.confirmedHasDraft = false;
    this.hasDraft = false;
    this.configName = '';
    this.baseRev = null;
    this.revs = [];
    this.baseline = null;
    this.saveState = 'clean';
    this.savedAt = null;
    this.errors = [];
    this.warnings = [];
    this.usage = null;
    this.hash = null;
    this.pages = [];
    this.missingPages = [];
    this.past = [];
    this.future = [];
    this.lastEditKey = null;
    this.offline = false;
    this.retries = 0;
    this.stashed = null;
    this.emit();
    await this.load();
  }

  /** The `configs` push no longer lists the open config: confirm with the server, then stop autosaving. */
  async checkDeleted(): Promise<void> {
    if (this.deleted || this.leaving || !this.loaded) return;
    const gen = this.gen;
    try {
      await this.api.getConfig(this.configId);
    } catch (e) {
      if (gen !== this.gen || this.leaving) return;
      if (e instanceof ApiError && e.status === 404) this.markDeleted();
    }
  }
  markDeleted(): void {
    this.deleted = true;
    for (const t of [this.timer, this.retryTimer, this.previewTimer]) if (t) clearTimeout(t);
    this.timer = this.retryTimer = null;
    this.writeStash();
    this.emit();
  }
  /** Create a config from YAML text and switch to it. */
  async importAsNew(name: string, yaml: string): Promise<string> {
    const r = await this.api.createConfig({ name, yaml });
    await this.switchTo(r.id);
    return r.id;
  }
  /** Deleted banner: keep the current edits as a new config and open it. */
  async saveAsNew(name: string): Promise<string> {
    const config = clone(this.config);
    const r = await this.api.createConfig({ name });
    await this.api.putDraft(r.id, config);
    this.clearStash();
    await this.switchTo(r.id);
    return r.id;
  }

  async rename(name: string): Promise<void> {
    const r = await this.api.renameConfig(this.configId, name);
    this.configName = r.name;
    this.emit();
  }
}

/** "my-home_setup.yaml" -> "My Home Setup" */
export function nameFromFile(file: string): string {
  const base = file.replace(/\.[^.]*$/, '').replace(/[-_.\s]+/g, ' ').trim();
  const t = base.replace(/\b([a-z])/g, (_m, c: string) => c.toUpperCase());
  return (t || 'Imported Config').slice(0, 80);
}

let current: Store | null = null;
export const setStore = (s: Store) => (current = s);
export const getStore = (): Store => {
  if (!current) throw new Error('store not initialised');
  return current;
};
