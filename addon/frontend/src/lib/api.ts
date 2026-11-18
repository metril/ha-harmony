// Typed client for the add-on API. All URLs are relative to document.baseURI (ingress prefix).
import type { Config, IrAction, ValidationError } from './model';

export interface Meta {
  applied: { rev: number; at: number; hash: string } | null;
  history: { rev: number; at: number; hash: string }[];
  history_max: number;
  /** integer draft revision (separate from applied.rev); sent back as If-Match */
  rev: number;
}
export interface ConfigResponse extends Meta {
  hub: string;
  draft: { config: Config; updated: number } | null;
  applied: (NonNullable<Meta['applied']> & { config?: Config }) | null;
}
export interface PutResponse extends Meta {
  hub: string;
  draft_updated: number;
}
export interface Usage {
  json_bytes: number;
  json_max: number;
  pool_words: number;
  pool_max: number;
  keys: number;
  keys_max: number;
  devices: number;
  devices_max: number;
  activities: number;
  activities_max: number;
  commands: number;
  commands_max: number;
  inputs: number;
  inputs_max: number;
  soft_keys: number;
  soft_keys_max: number;
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
  icon: number;
  bind: number | null;
}
export interface V2Page {
  version: 2;
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
/** config/status: the integration has applied the bundle once acked_rev === applied_rev. */
export interface ApplyStatus {
  hub: string;
  applied_rev: string | null;
  acked_rev: string | null;
  result: 'ok' | 'error' | null;
  detail: string | null;
  acked_at: number | null;
  linked: boolean;
  last_seen: number | null;
}
export interface HubInfo {
  id: string;
  has_draft: boolean;
  has_applied: boolean;
  applied_rev: string | null;
  applied_at: number | null;
}
export interface HistoryEntry {
  hub: string;
  rev: number;
  at: number;
  hash: string;
  config: Config;
}
export interface HaEntity {
  entity_id: string;
  name?: string;
  domain?: string;
  state?: string;
  device_class?: string;
}
export interface HaService {
  domain: string;
  service: string;
  name?: string;
  fields?: string[];
}

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
    opts: { query?: Record<string, string | undefined>; body?: unknown; yaml?: string; headers?: Record<string, string> } = {},
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
      res = await this.doFetch(this.href(path, opts.query), { method, headers, body, credentials: 'same-origin' });
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

  health = () => this.request<{ ok: boolean; addon_version: string }>('GET', 'api/v1/health');
  hubs = () => this.request<{ hubs: HubInfo[] }>('GET', 'api/v1/hubs');
  getConfig = (hub: string, full = false) =>
    this.request<ConfigResponse>('GET', 'api/v1/config', { query: { hub, full: full ? '1' : undefined } });
  /** The source config of the applied version or one history row, by its integer rev. */
  historyEntry = (hub: string, rev: number) =>
    this.request<HistoryEntry>('GET', 'api/v1/config/history', { query: { hub, rev: String(rev) } });
  putDraft = (hub: string, config: Config, rev?: number | null) =>
    this.request<PutResponse>('PUT', 'api/v1/config/draft', {
      query: { hub },
      body: { config },
      headers: rev != null ? { 'If-Match': String(rev) } : {},
    });
  validate = (hub: string, config?: Config) =>
    this.request<{ ok: boolean; errors: ValidationError[] }>('POST', 'api/v1/config/validate', {
      query: { hub },
      body: config ? { config } : {},
    });
  /** Always 200; look at `ok` and `errors`. Without `config` it checks the saved draft. */
  preview = (hub: string, config?: Config) =>
    this.request<Preview>('POST', 'api/v1/config/preview', { query: { hub }, body: config ? { config } : {} });
  apply = (hub: string) => this.request<Meta & { ok: true }>('POST', 'api/v1/config/apply', { query: { hub } });
  revert = (hub: string, target: 'applied' | 'previous') =>
    this.request<Meta & { ok: true }>('POST', 'api/v1/config/revert', { query: { hub }, body: { target } });
  importYaml = (hub: string, yaml: string, rev?: number | null) =>
    this.request<PutResponse & { ok: true }>('POST', 'api/v1/config/import_yaml', {
      query: { hub },
      yaml,
      headers: rev != null ? { 'If-Match': String(rev) } : {},
    });
  exportUrl = (hub: string, source?: 'draft' | 'applied') => this.href('api/v1/config/export_yaml', { hub, source });
  applyStatus = (hub: string) => this.request<ApplyStatus>('GET', 'api/v1/config/status', { query: { hub } });
  /** 503 ha_unavailable means Home Assistant cannot be read; callers fall back to typed ids. */
  haEntities = (domain?: string) =>
    this.request<{ entities: HaEntity[] }>('GET', 'api/v1/ha/entities', { query: { domain } });
  haServices = (domain?: string) =>
    this.request<{ services: HaService[] }>('GET', 'api/v1/ha/services', { query: { domain } });
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
    private hub: string,
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
        const res = await this.api.putDraft(this.hub, job.config, force ? null : this.rev);
        this.rev = res.rev;
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
