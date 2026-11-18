// Typed client for the add-on API. All URLs are relative to document.baseURI (ingress prefix).
import type { Config, ValidationError } from './model';

export interface Meta {
  applied: { rev: number; at: number; hash: string } | null;
  /** `config` is not returned today; used for "Restore as draft" when the API adds it. */
  history: { rev: number; at: number; hash: string; config?: Config }[];
  history_max: number;
}
export interface ConfigResponse extends Meta {
  hub: string;
  draft: { config: Config; updated: number; rev?: number } | null;
  applied: (Meta['applied'] & { config?: Config }) | null;
}
export interface PutResponse extends Meta {
  hub: string;
  draft_updated: number;
  draft_rev?: number;
  rev?: number;
}
export interface Usage {
  json_bytes: number;
  json_max: number;
  pool_words: number;
  pool_max: number;
  devices: number;
  devices_max: number;
  activities: number;
  activities_max: number;
}
export interface Preview {
  ok: true;
  hash: string;
  usage: Usage;
  pages: unknown[];
}
export interface ApplyStatus {
  rev: number;
  state: 'pending' | 'ok' | 'error';
  detail?: string;
}
export interface HaEntity {
  entity_id: string;
  name?: string;
  state?: string;
}
export interface HaService {
  domain: string;
  service: string;
  name?: string;
}

export class ApiError extends Error {
  constructor(
    public status: number,
    public code: string,
    msg: string,
    public errors: ValidationError[] = [],
    public files: string[] = [],
  ) {
    super(msg);
    this.name = 'ApiError';
  }
  get isConflict(): boolean {
    return this.status === 409 && ['conflict', 'rev_conflict', 'stale_draft', 'rev_mismatch'].includes(this.code);
  }
  get isNetwork(): boolean {
    return this.status === 0;
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
      const d = (data ?? {}) as { error?: { code?: string; msg?: string; errors?: ValidationError[]; files?: string[] }; errors?: ValidationError[] };
      const err = d.error ?? {};
      throw new ApiError(
        res.status,
        err.code ?? 'http_error',
        err.msg ?? `Request failed (${res.status})`,
        err.errors ?? d.errors ?? [],
        err.files ?? [],
      );
    }
    return data as T;
  }

  health = () => this.request<{ ok: boolean; addon_version: string }>('GET', 'api/v1/health');
  /** Not in the API yet: callers treat 404/503 as "single hub". */
  hubs = () => this.request<{ hubs: { id: string; name?: string }[] }>('GET', 'api/v1/hubs');
  getConfig = (hub: string, full = false) =>
    this.request<ConfigResponse>('GET', 'api/v1/config', { query: { hub, full: full ? '1' : undefined } });
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
  /** 200 with usage, or ApiError 422 carrying `errors`. */
  preview = (hub: string, config?: Config) =>
    this.request<Preview>('POST', 'api/v1/config/preview', { query: { hub }, body: config ? { config } : {} });
  apply = (hub: string) => this.request<Meta & { ok: true }>('POST', 'api/v1/config/apply', { query: { hub } });
  revert = (hub: string, target: 'applied' | 'previous') =>
    this.request<Meta & { ok: true }>('POST', 'api/v1/config/revert', { query: { hub }, body: { target } });
  importYaml = (hub: string, yaml: string) =>
    this.request<PutResponse & { ok: true }>('POST', 'api/v1/config/import_yaml', { query: { hub }, yaml });
  exportUrl = (hub: string, source?: 'draft' | 'applied') => this.href('api/v1/config/export_yaml', { hub, source });
  /** Planned: GET config/status. Callers treat 404 as "no status available". */
  applyStatus = (hub: string) => this.request<ApplyStatus>('GET', 'api/v1/config/status', { query: { hub } });
  /** Amendment 9: 503 means Home Assistant is unreachable; callers fall back to typed ids. */
  haEntities = (domain?: string) =>
    this.request<{ entities: HaEntity[] }>('GET', 'api/v1/ha/entities', { query: { domain } });
  haServices = (domain?: string) =>
    this.request<{ services: HaService[] }>('GET', 'api/v1/ha/services', { query: { domain } });
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
 * sent as If-Match on the next one (when the API returns one).
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
        const r = res.draft_rev ?? res.rev;
        if (typeof r === 'number') this.rev = r;
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
