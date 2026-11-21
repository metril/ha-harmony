// Typed client for the add-on API. All URLs are relative to document.baseURI (ingress prefix).
import type { Config, Device, IrAction, ValidationError } from './model';
import { Live } from './live';

/** A config library row (`GET configs`, and the head of every config response). */
export interface ConfigSummary {
  id: string;
  name: string;
  created: number;
  updated: number;
  latest_rev: number | null;
  rev_count: number;
  /** autosave counter, sent back as If-Match; unrelated to revision numbers */
  draft_rev: number;
  has_draft: boolean;
  draft_updated: number | null;
  base_rev: number | null;
  dirty: boolean;
  hubs?: { hub: string; rev: number }[];
}
export interface RevInfo {
  rev: number;
  at: number;
  note: string;
  hash: string;
  parent: number | null;
}
export interface ConfigDetail extends ConfigSummary {
  draft: { config: Config; updated: number; base_rev: number | null } | null;
  revs: RevInfo[];
  hubs: { hub: string; rev: number }[];
}
export interface RevDetail extends RevInfo {
  id: string;
  config: Config;
}
export interface PutResponse {
  id: string;
  draft_updated: number;
  draft_rev: number;
  base_rev: number | null;
  dirty: boolean;
}
export interface AppliedVersion {
  config_id: string;
  rev: number;
  name: string;
  at: number;
  hash: string;
}
/** `GET hubs/{hub}` and the answer of apply / revert */
export interface HubState {
  hub: string;
  applied: AppliedVersion | null;
  history: AppliedVersion[];
  history_max: number;
  config_exists?: boolean;
  ack?: { rev: string; result: string; at: number; detail?: string } | null;
}
export interface Usage {
  json_bytes: number;
  pool_words: number;
  keys: number;
  keys_max: number;
  devices: number;
  devices_max: number;
  activities: number;
  activities_max: number;
  commands: number;
  inputs: number;
  /** a current hub: what loading this config needs of its memory, and the budget it allows (bytes) */
  config_bytes: number;
  config_budget: number;
  /** the fixed part of a load the hub reports (slack_b); the meter shows config_bytes and config_budget without it */
  config_overhead?: number | null;
  soft_keys: number;
  soft_keys_max: number;
  /** wire pages (a long list page counts each of its pages) and the remote's cap */
  pages: number;
  page_limit: number;
}
/** page-format-v2 model (harmony_core.page_to_dict): what the editor renders. */
export interface V2Widget {
  kind: 'button' | 'tile' | 'label';
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  label: string;
  role: string;
  shape: string;
  icon_only: boolean;
  /** icon + label group alignment; absent = centre (the default, for every shape) */
  align?: 'left' | 'right';
  /** draw the nav chevron at the right edge */
  trailing?: boolean;
  icon: number;
  bind: number | null;
  /** palette name or `#RRGGBB`; null = the role's colour */
  color?: string | null;
  /** tiles: the colour while on */
  on_color?: string | null;
}
export interface V2Page {
  version: 1 | 2;
  id: number;
  cols: number;
  rows: number;
  gen: number;
  total: number;
  kind: 'normal' | 'home' | 'favorites';
  back: boolean;
  seq: { idx: number; cnt: number } | null;
  act: number;
  title: string;
  /** theme preset id (7 = the config's own theme) */
  theme?: number;
  widgets: V2Widget[];
}
/** One wire page of a config page; a long `list:` page compiles to several ("2/3"). */
export interface WirePage {
  page_id: number;
  seq: { idx: number; cnt: number } | null;
  bytes: number;
  bytes_v1: number;
  widgets: number;
  page: V2Page;
}
/** One entry per config page. `bytes`/`limit` use the v2 size rule; `bytes_v1` is the v1 size. */
export interface PreviewPage {
  index: number;
  id: string;
  ok: boolean;
  error: string | null;
  page: V2Page | null;
  wire_pages: WirePage[];
  bytes: number | null;
  bytes_v1: number | null;
  limit: number;
  widgets: number | null;
  widget_limit: number;
}
/** Always HTTP 200: `errors` lists what is wrong, `usage`/`hash` are null unless the whole config is valid. */
export interface Preview {
  ok: boolean;
  hash: string | null;
  errors: ValidationError[];
  missing_pages: { index: number; id: string }[];
  pages: PreviewPage[];
  usage: Usage | null;
}
/** The `hub:<id>` live topic: the integration has applied the bundle once acked_rev === applied_rev. */
export interface ApplyStatus {
  hub: string;
  applied_rev: string | null;
  config_id?: string | null;
  config_name?: string | null;
  rev?: number | null;
  acked_rev: string | null;
  result: 'ok' | 'error' | null;
  detail: string | null;
  acked_at: number | null;
  /** the integration's channel for this hub is open */
  connected: boolean;
  connected_at?: number | null;
  /** add-on clock, unix seconds */
  now?: number;
  /** what the linked remote's firmware cannot show (colours / custom themes), when the integration reports it */
  theme_notices?: string[];
  /** the linked remotes as the integration last reported them (colour tier and what it costs) */
  remotes?: { remote: number; caps: number; tier: 'none' | 'palette' | 'custom'; notices: string[] }[];
}
export interface HubsResponse {
  hubs: HubInfo[];
  /** `name`: the hub's display name, when its integration reported one */
  linked: { id: string; last_seen: number; name?: string }[];
  now?: number;
  /** a store under `default` that can move to one of these linked ids */
  adopt: { from: string; to: string[] } | null;
}
export interface HubInfo {
  id: string;
  has_applied: boolean;
  config_id?: string | null;
  config_name?: string | null;
  /** revision of the config the hub runs */
  rev?: number | null;
  applied_rev: string | null;
  applied_at: number | null;
}
export interface ConfigsResponse {
  configs: ConfigSummary[];
}
export interface HaEntity {
  entity_id: string;
  name?: string;
  domain?: string;
  state?: string;
  device_class?: string;
  /** the registry ids the add-on sends (absent when Home Assistant's registries cannot be read) */
  area_id?: string;
  device_id?: string;
  /** the integration that provides it */
  platform?: string;
  label_ids?: string[];
  /** a diagnostic or config entity (only listed when asked for) */
  diag?: boolean;
  /** filled in by the client from the catalog's lookups, for display and search only */
  area?: string;
  device?: string;
  labels?: string[];
  floor_id?: string;
}
export interface HaCatalog {
  entities: HaEntity[];
  areas: { id: string; name: string; floor_id?: string; floor?: string }[];
  devices?: { id: string; name: string }[];
  labels?: { id: string; name: string }[];
  /** false: only the REST data (type grouping), no areas, devices or integrations */
  registries: boolean;
  /** entities that matched before the cap, and whether the list was cut at it */
  total?: number;
  truncated?: boolean;
}
export interface HaService {
  domain: string;
  service: string;
  name?: string;
  fields?: string[];
}

/** One hard key's effective action in one layer (config/keymap). */
export interface KeymapRow {
  key: string;
  code: number;
  source: 'default' | 'activity' | 'none';
  action: ({ kind: string; label: string } & Record<string, unknown>) | null;
}
export interface Keymap {
  ok: boolean;
  default: KeymapRow[];
  activities: { id: string; name: string; keys: KeymapRow[] }[];
}
export interface IrBrand {
  name: string;
  slug: string;
  count: number;
}
export interface IrModel {
  file: string;
  name: string;
  id: number;
}
export interface IrModels {
  brand: { name: string; slug: string };
  total: number;
  offset: number;
  limit: number;
  models: IrModel[];
}
export interface IrDeviceResult {
  device: Device;
  forms: Record<string, string>;
  notes: string[];
  skipped: { name: string; reason: string }[];
  skipped_total: number;
  device_type: { id: string; name: string } | null;
  source: Record<string, unknown>;
}
export interface IrCandidate {
  kind: 'on' | 'off' | 'toggle';
  action: IrAction;
  form: string;
  count: number;
  models: string[];
  /** archive file of each example model (older add-ons omit it) */
  model_files?: string[];
  codesets: number;
}
export interface IrCandidates {
  candidates: IrCandidate[];
  examined: number;
  matched: number;
  errors: number;
  truncated?: boolean;
}
export interface IrParsed {
  ok: boolean;
  actions: { name: string; action: IrAction; form: string }[];
  skipped: { name: string; reason: string }[];
}
export interface Health {
  ok: boolean;
  addon_version: string;
  /** what this add-on build can store; absent on older builds */
  capabilities?: { ha_actions?: boolean; page_format?: number };
}

export type BtLinkState = 'off' | 'starting' | 'ready' | 'no_adapter' | 'disabled';
export interface BtHost {
  slot: number;
  addr: string;
  name?: string;
  transport: 'classic' | 'le';
  connected: boolean;
  configured?: boolean;
}
export interface BtRemote {
  slot: number;
  addr: string;
  name?: string;
  connected: boolean;
  battery?: number;
  configured?: boolean;
}
export interface BtState {
  state: BtLinkState;
  discoverable: boolean;
  discoverable_secs?: number;
  le?: boolean;
  gen?: number;
  window?: 'host' | 'remote';
  pairing?: { result: 'ok' | 'failed' | 'refused'; addr?: string; name?: string; transport?: 'classic' | 'le'; kind?: 'host' | 'remote' };
  hosts: BtHost[];
  remotes: BtRemote[];
  candidates?: { addr: string; name?: string; rssi: number }[];
}
export interface BtLimits {
  hosts_max: number;
  conn_max: number;
  remotes_max: number;
  le_bonds_max: number;
  pages: number[];
  le: boolean;
}
export interface BtStatus {
  hub: string;
  status: 'ok' | 'unsupported' | 'offline' | 'unknown';
  state: BtState | null;
  limits: BtLimits | null;
  listening: boolean;
}
export interface BtCodes {
  builtins: { name: string; page: number; usage: number; default: boolean }[];
  remote_keys: { name: string; page: number; usage: number; key: string }[];
  keys: string[];
}

export const KEEPALIVE_MAX = 60 * 1024;
export class ApiError extends Error {
  constructor(
    public status: number,
    public code: string,
    msg: string,
    public errors: ValidationError[] = [],
    public files: string[] = [],
    /** the rest of the error object: `rev` on a 409 conflict, `retry_after_ms` on a 429 */
    public extra: Record<string, unknown> = {},
  ) {
    super(msg);
    this.name = 'ApiError';
  }
  get isConflict(): boolean {
    return this.status === 409 && this.code === 'conflict';
  }
  get isNetwork(): boolean {
    return this.status === 0;
  }
  get retryAfterMs(): number | null {
    const v = this.extra['retry_after_ms'];
    return typeof v === 'number' ? v : null;
  }
}

type Fetch = typeof fetch;
const MUTATING = new Set(['POST', 'PUT', 'PATCH', 'DELETE']);

export class Api {
  private _live?: Live;
  /** the one editor WebSocket (opens when something subscribes) */
  get live(): Live {
    return (this._live ??= new Live(() => this.href('api/v1/live').replace(/^http/, 'ws')));
  }
  constructor(
    private base: string = document.baseURI,
    private doFetch: Fetch = (...a) => fetch(...a),
  ) {}

  href(path: string, query: Record<string, string | undefined> = {}): string {
    const u = new URL(path.replace(/^\/+/, ''), this.base);
    for (const [k, v] of Object.entries(query)) if (v !== undefined) u.searchParams.set(k, v);
    return u.toString();
  }

  async request<T>(
    method: string,
    path: string,
    opts: { query?: Record<string, string | undefined>; body?: unknown; yaml?: string; headers?: Record<string, string>; keepalive?: boolean } = {},
  ): Promise<T> {
    const headers: Record<string, string> = { Accept: 'application/json', ...opts.headers };
    let body: string | undefined;
    if (opts.yaml !== undefined) {
      headers['Content-Type'] = 'text/yaml';
      body = opts.yaml;
    } else if (opts.body !== undefined) {
      headers['Content-Type'] = 'application/json';
      body = JSON.stringify(opts.body);
    } else if (MUTATING.has(method)) {
      headers['Content-Type'] = 'application/json';
      body = '{}';
    }
    if (MUTATING.has(method)) headers['X-HH-CSRF'] = '1';
    let res: Response;
    try {
      res = await this.doFetch(this.href(path, opts.query), { method, headers, body, credentials: 'same-origin', ...(opts.keepalive ? { keepalive: true } : {}) });
    } catch {
      throw new ApiError(0, 'network', 'Cannot reach the add-on. Check your connection and retry.');
    }
    const text = await res.text();
    let data: unknown = null;
    if (text) {
      try {
        data = JSON.parse(text);
      } catch {
        data = text;
      }
    }
    if (!res.ok) {
      // {error:{code,msg,...extra}}; apply's 422 is the one flat shape: {ok:false, errors}
      const d = (data ?? {}) as { error?: { code?: string; msg?: string; errors?: ValidationError[]; files?: string[] } & Record<string, unknown>; errors?: ValidationError[] };
      const { code, msg, errors, files, ...extra } = d.error ?? {};
      throw new ApiError(res.status, code ?? 'http_error', msg ?? `Request failed (${res.status})`, errors ?? d.errors ?? [], files ?? [], extra);
    }
    return data as T;
  }

  health = () => this.request<Health>('GET', 'api/v1/health');
  hubs = async (): Promise<HubsResponse> => {
    const r = await this.request<Partial<HubsResponse> & { hubs: HubInfo[] }>('GET', 'api/v1/hubs');
    return { linked: [], adopt: null, ...r };
  };
  adoptHub = (to: string, from = 'default') => this.request<{ ok: true; hub: string }>('POST', 'api/v1/hubs/adopt', { body: { from, to } });
  configs = () => this.request<ConfigsResponse>('GET', 'api/v1/configs');
  getConfig = (id: string) => this.request<ConfigDetail>('GET', `api/v1/configs/${encodeURIComponent(id)}`);
  createConfig = (body: { name: string; from?: { id: string; rev?: number }; yaml?: string }) =>
    this.request<ConfigDetail & { ok: true }>('POST', 'api/v1/configs', { body });
  renameConfig = (id: string, name: string) => this.request<ConfigDetail & { ok: true }>('PATCH', `api/v1/configs/${encodeURIComponent(id)}`, { body: { name } });
  deleteConfig = (id: string) => this.request<{ ok: true }>('DELETE', `api/v1/configs/${encodeURIComponent(id)}`);
  getRev = (id: string, rev: number) => this.request<RevDetail>('GET', `api/v1/configs/${encodeURIComponent(id)}/revs/${rev}`);
  saveRev = (id: string, note = '') =>
    this.request<PutResponse & { ok: true; rev: RevInfo }>('POST', `api/v1/configs/${encodeURIComponent(id)}/revs`, { body: note ? { note } : {} });
  restoreRev = (id: string, rev: number) => this.request<PutResponse & { ok: true }>('POST', `api/v1/configs/${encodeURIComponent(id)}/restore`, { body: { rev } });
  putDraft = (id: string, config: Config, rev?: number | null) =>
    this.request<PutResponse>('PUT', `api/v1/configs/${encodeURIComponent(id)}/draft`, {
      body: { config },
      headers: rev != null ? { 'If-Match': String(rev) } : {},
    });
  /** Fire-and-forget PUT for page unload: `keepalive` lets it outlive the page. Over KEEPALIVE_MAX bytes browsers refuse
   *  keepalive bodies (64 KiB), so a normal fetch is used and may be cut off. */
  putDraftOnUnload = (id: string, config: Config, rev?: number | null): Promise<unknown> =>
    this.request<PutResponse>('PUT', `api/v1/configs/${encodeURIComponent(id)}/draft`, {
      body: { config },
      headers: rev != null ? { 'If-Match': String(rev) } : {},
      keepalive: JSON.stringify(config).length < KEEPALIVE_MAX,
    }).catch(() => undefined);
  importYaml = (id: string, yaml: string, rev?: number | null) =>
    this.request<PutResponse & { ok: true }>('POST', `api/v1/configs/${encodeURIComponent(id)}/import_yaml`, {
      yaml,
      headers: rev != null ? { 'If-Match': String(rev) } : {},
    });
  exportUrl = (id: string, source: 'draft' | `rev:${number}` = 'draft') => this.href(`api/v1/configs/${encodeURIComponent(id)}/export_yaml`, { source });
  // stateless: the config is always in the body; `hub` only picks the limits the hub's integration reported
  validate = (hub: string, config: Config) =>
    this.request<{ ok: boolean; errors: ValidationError[] }>('POST', 'api/v1/config/validate', { query: { hub }, body: { config } });
  /** Always 200; look at `ok` and `errors`. */
  preview = (hub: string, config: Config) => this.request<Preview>('POST', 'api/v1/config/preview', { query: { hub }, body: { config } });
  /** Effective hard-key action per layer; 422 when the config does not validate. */
  keymap = (hub: string, config: Config) => this.request<Keymap>('POST', 'api/v1/config/keymap', { query: { hub }, body: { config } });
  hubState = (hub: string) => this.request<HubState>('GET', `api/v1/hubs/${encodeURIComponent(hub)}`);
  /** Without `rev` the config's draft is saved as a revision (or the newest reused) and applied. */
  applyToHub = (hub: string, configId: string, rev?: number) =>
    this.request<HubState & { ok: true }>('POST', `api/v1/hubs/${encodeURIComponent(hub)}/apply`, { body: { config_id: configId, ...(rev !== undefined ? { rev } : {}) } });
  revertHub = (hub: string) => this.request<HubState & { ok: true }>('POST', `api/v1/hubs/${encodeURIComponent(hub)}/revert`, { body: {} });
  /** 503 ha_unavailable means Home Assistant cannot be read; callers fall back to typed ids. */
  haEntities = (domain?: string, diagnostic = false, state = false) =>
    this.request<HaCatalog>('GET', 'api/v1/ha/entities', { query: { domain, diagnostic: diagnostic ? '1' : undefined, state: state ? '1' : undefined } });
  haServices = (domain?: string) =>
    this.request<{ services: HaService[] }>('GET', 'api/v1/ha/services', { query: { domain } });
  irBrands = (q = '') => this.request<{ brands: IrBrand[] }>('GET', 'api/v1/ir/brands', { query: { q: q || undefined } });
  irModels = (brand: string, q = '', offset = 0, limit = 50) =>
    this.request<IrModels>('GET', 'api/v1/ir/models', { query: { brand, q: q || undefined, offset: String(offset), limit: String(limit) } });
  irDevice = (brand: string, model: string) => this.request<IrDeviceResult>('GET', 'api/v1/ir/device', { query: { brand, model } });
  irCandidates = (brand: string, type: string, limit = 60) =>
    this.request<IrCandidates>('GET', 'api/v1/ir/candidates', { query: { brand, type, limit: String(limit) } });
  irParse = (kind: 'pronto' | 'flipper' | 'smartir', text: string) =>
    this.request<IrParsed>('POST', 'api/v1/ir/parse', { body: { kind, text } });
  // Bluetooth relay (docs/addon-api.md "Bluetooth relay"). Commands answer once the integration has run them on the hub.
  btCodes = () => this.request<BtCodes>('GET', 'api/v1/bt/codes');
  // `cmdId` is one id per user action: re-send the same one when retrying after a timeout so the command never runs twice.
  btPair = (hub: string, mode: 'host' | 'remote', transport: 'any' | 'classic' | 'le' = 'any', secs = 120, cmdId?: string) =>
    this.request<{ ok: true; state: BtState | null }>('POST', 'api/v1/bt/pair', { body: { hub, mode, transport, secs, ...(cmdId ? { cmd_id: cmdId } : {}) } });
  btPairStop = (hub: string, cmdId?: string) => this.request<{ ok: true }>('POST', 'api/v1/bt/pair/stop', { body: { hub, ...(cmdId ? { cmd_id: cmdId } : {}) } });
  btPick = (hub: string, addr: string, cmdId?: string) => this.request<{ ok: true }>('POST', 'api/v1/bt/pick', { body: { hub, addr, ...(cmdId ? { cmd_id: cmdId } : {}) } });
  btForget = (hub: string, slot: number, kind: 'host' | 'remote', cmdId?: string) =>
    this.request<{ ok: true }>('POST', 'api/v1/bt/forget', { body: { hub, slot, kind, ...(cmdId ? { cmd_id: cmdId } : {}) } });
  btKey = (hub: string, slot: number, page: number, usage: number, action: 'down' | 'up' | 'press' = 'press', cmdId?: string) =>
    this.request<{ ok: true }>('POST', 'api/v1/bt/key', { body: { hub, slot, page, usage, action, ...(cmdId ? { cmd_id: cmdId } : {}) } });
  /** Fire one inline code through Home Assistant. 429 carries retry_after_ms. */
  irTest = (hub: string, ir: IrAction, portMask?: number) =>
    this.request<{ ok: true; action: IrAction }>('POST', 'api/v1/ir/test', {
      body: { hub, ir, ...(portMask !== undefined ? { port_mask: portMask } : {}) },
    });
}

/** Entities, or null when Home Assistant cannot be read (503/404) so the UI offers free text. */
export async function haEntitiesOrNull(api: Api, domain?: string): Promise<HaEntity[] | null> {
  try {
    return (await api.haEntities(domain)).entities;
  } catch (e) {
    if (e instanceof ApiError && [0, 404, 503].includes(e.status)) return null;
    throw e;
  }
}
/** The catalog, or why it cannot be read (the 503's `reason`; '' when there is none, e.g. no add-on). */
export async function haCatalogOrReason(api: Api, diagnostic = false, state = false): Promise<{ catalog: HaCatalog } | { reason: string }> {
  try {
    return { catalog: await api.haEntities(undefined, diagnostic, state) };
  } catch (e) {
    if (e instanceof ApiError && [0, 404, 503].includes(e.status)) return { reason: typeof e.extra['reason'] === 'string' ? e.extra['reason'] : e.status === 0 ? 'NetworkError' : '' };
    throw e;
  }
}
export async function haServicesOrNull(api: Api, domain?: string): Promise<HaService[] | null> {
  try {
    return (await api.haServices(domain)).services;
  } catch (e) {
    if (e instanceof ApiError && [0, 404, 503].includes(e.status)) return null;
    throw e;
  }
}

/**
 * Single queue for draft PUTs. At most one request is in flight; edits that arrive meanwhile
 * collapse into one follow-up PUT carrying the newest config. The rev from each response is
 * sent as If-Match on the next one.
 */
export class DraftQueue {
  rev: number | null = null;
  private inflight = false;
  private pending: { config: Config; waiters: { ok: (r: PutResponse) => void; fail: (e: unknown) => void }[] } | null = null;
  constructor(
    private api: Api,
    private id: string,
  ) {}

  enqueue(config: Config, opts: { force?: boolean } = {}): Promise<PutResponse> {
    return new Promise((ok, fail) => {
      if (this.pending) {
        this.pending.config = config;
        this.pending.waiters.push({ ok, fail });
      } else {
        this.pending = { config, waiters: [{ ok, fail }] };
      }
      if (!this.inflight) void this.pump(opts.force ?? false);
    });
  }

  private async pump(force: boolean): Promise<void> {
    this.inflight = true;
    while (this.pending) {
      const job = this.pending;
      this.pending = null;
      try {
        const res = await this.api.putDraft(this.id, job.config, force ? null : this.rev);
        this.rev = res.draft_rev;
        job.waiters.forEach((w) => w.ok(res));
      } catch (e) {
        job.waiters.forEach((w) => w.fail(e));
        // after a failure later edits must not run on stale assumptions: drop them with the same error
        const rest = this.pending as typeof job | null;
        this.pending = null;
        rest?.waiters.forEach((w) => w.fail(e));
      }
      force = false;
    }
    this.inflight = false;
  }
}
