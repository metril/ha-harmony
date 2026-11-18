// UI state of the remote studio (what is selected, which layer, zoom...) plus the data the studio
// fetches around the draft: key map per layer, add-on capabilities, Home Assistant entities/services.
import { ApiError, haEntitiesOrNull, haServicesOrNull, type HaEntity, type HaService, type Keymap, type KeymapRow, type PreviewPage } from './api';
import type { Store } from './store';
import { DEFAULT_LAYER, keyActionLabel, pageIndexForLayer, type Layer } from './studio-model';

export type Sel = { kind: 'key'; name: string } | { kind: 'widget'; page: number; index: number } | { kind: 'page' } | null;
export type PaletteTab = 'devices' | 'activities' | 'home' | 'widgets';
export const HA_DOMAINS = ['light', 'switch', 'scene', 'script', 'media_player', 'remote', 'fan', 'cover', 'climate', 'input_boolean', 'input_button', 'button', 'number', 'select', 'sensor', 'binary_sensor'];

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
    this.kmTimer = setTimeout(() => void this.loadKeymap(), 120);
  }
  async loadKeymap(): Promise<void> {
    const seq = ++this.kmSeq;
    try {
      const km = await this.store.api.keymap(this.store.hub, structuredClone(this.store.config));
      if (seq !== this.kmSeq) return;
      const fix = (rows: typeof km.default) => rows.map((r) => (r.action ? { ...r, action: { ...r.action, label: keyActionLabel(r.action) } } : r));
      this.keymap = { ...km, default: fix(km.default), activities: km.activities.map((a) => ({ ...a, keys: fix(a.keys) })) };
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
  /** Entities and services for the Home tab and the action picker. null = Home Assistant cannot be read (typed ids fall back). */
  async loadHa(force = false): Promise<void> {
    if (this.haLoading || (this.haLoaded && !force)) return;
    this.haLoading = true;
    try {
      const [e, s] = await Promise.all([haEntitiesOrNull(this.store.api), haServicesOrNull(this.store.api)]);
      this.entities = e;
      this.services = s;
    } catch {
      this.entities = null;
      this.services = null;
    }
    this.haLoaded = true;
    this.haLoading = false;
    this.emit();
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
  /** commit a new box for a widget (move / resize) */
  setBox(page: number, index: number, box: { x: number; y: number; w: number; h: number }): void;
  nudge(page: number, index: number, dx: number, dy: number, resize: boolean): void;
  removeWidget(page: number, index: number): void;
  addWidgetFree(): void;
  openPopover(): void;
}
