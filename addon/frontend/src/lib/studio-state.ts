// UI state of the remote studio (what is selected, which layer, zoom...) plus the data the studio
// fetches around the draft: key map per layer, add-on capabilities, Home Assistant entities/services.
import { ApiError, haCatalogOrReason, haServicesOrNull, type HaEntity, type HaService, type Keymap, type KeymapRow, type PreviewPage } from './api';
import { enrich, noLookups, type Lookups } from './ha-nav';
import type { Store } from './store';
import { DEFAULT_LAYER, keyActionLabel, pageIndexForLayer, type Layer } from './studio-model';

export type Sel = { kind: 'key'; name: string } | { kind: 'widget'; page: number; index: number } | { kind: 'page' } | null;
export type PaletteTab = 'devices' | 'activities' | 'ha' | 'widgets';
export const HA_DOMAINS = ['light', 'switch', 'scene', 'script', 'media_player', 'remote', 'fan', 'cover', 'climate', 'input_boolean', 'input_button', 'button', 'number', 'select', 'sensor', 'binary_sensor'];

/** The add-on's summary of a device / activity / none action (harmony_core/keymap.summarize); null: leave it to the add-on. */
function clientSummary(a: unknown): KeymapRow['action'] {
  if (!a || typeof a !== 'object') return null;
  const x = a as Record<string, unknown>;
  if (typeof x['device'] === 'string' && Object.keys(x).length === 2) {
    if (typeof x['command'] === 'string') return { kind: 'device', device: x['device'], command: x['command'], label: `${x['device']}: ${x['command']}` };
    if (typeof x['input'] === 'string') return { kind: 'device', device: x['device'], input: x['input'], label: `${x['device']} -> ${x['input']}` };
  }
  if (typeof x['activity'] === 'string' && Object.keys(x).length === 1) return { kind: 'activity', activity: x['activity'], label: x['activity'] === 'off' ? 'Power off' : `Start ${x['activity']}` };
  if (x['none'] !== undefined && Object.keys(x).length === 1) return { kind: 'none', label: 'Does nothing' };
  return null;
}

export class StudioState extends EventTarget {
  layer: Layer = DEFAULT_LAYER;
  /** config page index shown on the screen */
  page = 0;
  /** wire page of a list page (the compiler splits long lists) */
  wire = 0;
  sel: Sel = null;
  xray = false;
  mode: 'remote' | 'screen' = 'remote';
  /** integer scale of the screen in Screen mode */
  scale = 2;
  zoom: 'fit' | '1' | '2' = 'fit';
  tab: PaletteTab = 'devices';
  query = '';
  /** text for the aria-live region */
  announcement = '';
  /** the page whose layer was last chosen automatically: following the layer until the user picks a page */
  followLayer = true;

  keymap: Keymap | null = null;
  keymapError: string | null = null;
  haAction: boolean | null = null;
  entities: HaEntity[] | null = null;
  services: HaService[] | null = null;
  haLoaded = false;
  haLoading = false;
  /** why the entities could not be read (the 503's `reason`), when `entities` is null */
  haReason = '';
  haLookups: Lookups = noLookups;
  /** the number of matching entities when the add-on cut the list at its cap, else 0 */
  haTruncated = 0;
  haAgain = false;
  haAt = 0;
  /** also ask for each entity's state (only the Overview shows it; it makes the answer change with every state) */
  haWithState = false;
  haRegistries = false;
  /** list diagnostic and config entities too (asked of the add-on, which leaves them out by default) */
  haDiag = false;
  private lastGood = new Map<string, PreviewPage>();
  private kmTimer: ReturnType<typeof setTimeout> | null = null;
  private kmSeq = 0;

  constructor(public store: Store) {
    super();
  }
  emit(): void {
    this.dispatchEvent(new Event('change'));
  }
  set(patch: Partial<Pick<StudioState, 'layer' | 'page' | 'wire' | 'sel' | 'xray' | 'mode' | 'scale' | 'zoom' | 'tab' | 'query'>>): void {
    Object.assign(this, patch);
    this.emit();
  }
  say(text: string): void {
    // alternate a trailing space so identical messages re-announce
    this.announcement = this.announcement === text ? text + ' ' : text;
    this.emit();
  }

  /** Switch the layer; the screen follows to that activity's page (or Home) unless the user chose a page. */
  setLayer(layer: Layer): void {
    this.layer = layer;
    this.page = pageIndexForLayer(this.store.config, layer);
    this.wire = 0;
    this.sel = null;
    this.say(layer === DEFAULT_LAYER ? 'Default layer' : `${this.layerName(layer)} layer`);
  }
  setPage(i: number, user = true): void {
    this.page = i;
    this.wire = 0;
    this.sel = user ? { kind: 'page' } : this.sel?.kind === 'widget' ? null : this.sel;
    this.emit();
  }
  layerName(layer: Layer): string {
    return layer === DEFAULT_LAYER ? 'Default' : ((this.store.config.activities ?? []).find((a) => a.id === layer)?.name ?? layer);
  }

  /** The page's last good preview: errors keep the previous render on the canvas. */
  previewOf(index: number): PreviewPage | null {
    const pp = this.store.pages[index];
    const id = this.store.config.pages?.[index]?.id;
    if (pp?.ok && pp.page && id) this.lastGood.set(id, pp);
    return (pp?.ok && pp.page ? pp : id ? this.lastGood.get(id) : null) ?? null;
  }
  rows(layer: Layer = this.layer): KeymapRow[] {
    if (!this.keymap) return [];
    if (layer === DEFAULT_LAYER) return this.keymap.default;
    return this.keymap.activities.find((a) => a.id === layer)?.keys ?? this.keymap.default;
  }

  // ------------------------------------------------------------ data around the draft
  /** Re-resolve the key layers soon after the config changes (about 20 ms on the add-on). */
  scheduleKeymap(): void {
    if (this.kmTimer) clearTimeout(this.kmTimer);
    if (this.predictKeymap()) this.emit(); // show explicit key actions at once; the add-on's answer reconciles
    this.kmTimer = setTimeout(() => void this.loadKeymap(), 60);
  }
  /** Optimistic key map: rows for keys the config sets itself, summarised the way the add-on does. */
  predictKeymap(): boolean {
    const km = this.keymap;
    if (!km) return false;
    const cfg = this.store.config;
    let changed = false;
    const patch = (rows: KeymapRow[], keys: Record<string, unknown> | undefined, source: 'default' | 'activity') => {
      for (const row of rows) {
        const a = keys?.[row.key];
        if (a === undefined) continue;
        const action = clientSummary(a);
        if (!action) continue;
        if (row.source !== source || JSON.stringify(row.action) !== JSON.stringify(action)) {
          row.source = source;
          row.action = action;
          changed = true;
        }
      }
    };
    patch(km.default, cfg.keys, 'default');
    for (const act of km.activities) patch(act.keys, (cfg.activities ?? []).find((x) => x.id === act.id)?.keys, 'activity');
    return changed;
  }
  /** friendly names of the Home Assistant entities read so far */
  entityNames(): Map<string, string> {
    return new Map((this.entities ?? []).filter((e) => e.name).map((e) => [e.entity_id, String(e.name)]));
  }
  /** Readable label of a key map action: "Lamp · toggle", "Shield · DPAD_UP". */
  actionLabel(a: NonNullable<KeymapRow['action']>): string {
    return keyActionLabel(a, this.store.config, this.entityNames());
  }
  async loadKeymap(): Promise<void> {
    const seq = ++this.kmSeq;
    try {
      const km = await this.store.api.keymap(this.store.hub, structuredClone(this.store.config));
      if (seq !== this.kmSeq) return;
      this.keymap = km;
      // friendly names for the labels of Home Assistant actions
      if (!this.haLoaded && [...km.default, ...km.activities.flatMap((a) => a.keys)].some((r) => r.action?.kind === 'ha' && 'service' in r.action)) void this.loadHa();
      this.keymapError = null;
    } catch (e) {
      if (seq !== this.kmSeq) return;
      // an invalid config has no key map: keep showing the last one, say why
      this.keymapError = e instanceof ApiError ? (e.errors[0]?.msg ?? e.message) : 'Could not reach the add-on';
    }
    this.emit();
  }
  async loadCapabilities(): Promise<void> {
    try {
      const h = await this.store.api.health();
      this.haAction = h.capabilities?.ha_actions === true;
    } catch {
      this.haAction = false;
    }
    this.emit();
  }
  /** Entities and services for the Home Assistant tab and the action picker. null = Home Assistant cannot be read (typed ids fall back). */
  async loadHa(force = false): Promise<void> {
    if (this.haLoading) {
      if (force) this.haAgain = true; // a request that arrives mid-load (Retry, the diagnostic toggle) is not lost
      return;
    }
    if (this.haLoaded && !force) return;
    this.haLoading = true;
    this.haAgain = false;
    this.emit();
    try {
      const [e, s] = await Promise.all([haCatalogOrReason(this.store.api, this.haDiag, this.haWithState), haServicesOrNull(this.store.api)]);
      if ('catalog' in e) {
        const r = enrich(e.catalog);
        this.entities = r.entities;
        this.haLookups = r.lookups;
        this.haRegistries = e.catalog.registries === true;
        this.haTruncated = e.catalog.truncated ? (e.catalog.total ?? r.entities.length) : 0;
        this.haReason = '';
      } else {
        this.entities = null;
        this.haReason = e.reason;
      }
      this.services = s;
    } catch {
      this.entities = null;
      this.services = null;
      this.haReason = '';
    }
    this.haLoaded = true;
    this.haLoading = false;
    this.haAt = Date.now();
    this.emit();
    if (this.haAgain) void this.loadHa(true);
  }
  /** Reload when the list is older than `ms` (the tab was opened or the window focused again; a 304 makes it cheap). */
  refreshHa(ms = 60000): void {
    if (this.haLoaded && Date.now() - this.haAt > ms) void this.loadHa(true);
  }
  setHaDiag(on: boolean): void {
    if (on === this.haDiag) return;
    this.haDiag = on;
    void this.loadHa(true);
  }
  get haWritable(): boolean {
    return this.haAction === true;
  }
}

/** What the canvas, palette and inspector may ask the studio to do (implemented by hh-studio). */
export interface StudioHost {
  state: StudioState;
  /** a hard key was clicked / activated: select it, or drop the picked-up source on it */
  activateKey(name: string): void;
  activateWidget(page: number, index: number): void;
  activateCell(page: number, x: number, y: number): void;
  /** clear the selected key or widget and close its popover */
  deselect(): void;
  /** commit a new box for a widget (move / resize) */
  setBox(page: number, index: number, box: { x: number; y: number; w: number; h: number }): void;
  nudge(page: number, index: number, dx: number, dy: number, resize: boolean): void;
  removeWidget(page: number, index: number): void;
  addWidgetFree(): void;
  openPopover(): void;
}
