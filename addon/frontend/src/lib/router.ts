// Tiny hash router: #/devices/3/commands. The hub is ?hub= (part of the URL, outside the hash).
export interface Route {
  section: string;
  parts: string[];
}
export const SECTIONS = ['remote', 'overview', 'devices', 'activities', 'themes', 'ir', 'history'] as const;

export function parseHash(hash: string): Route {
  const parts = hash.replace(/^#\/?/, '').split('/').filter(Boolean).map(decodeURIComponent);
  const section = parts.shift() ?? 'remote';
  return { section: (SECTIONS as readonly string[]).includes(section) ? section : 'remote', parts };
}
export const hashFor = (...parts: (string | number)[]): string => '#/' + parts.map((p) => encodeURIComponent(String(p))).join('/');

export function hubFromSearch(search: string): string {
  const h = new URLSearchParams(search).get('hub');
  return h && /^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$/.test(h) ? h : 'default';
}

export class Router extends EventTarget {
  route: Route = parseHash(typeof location === 'undefined' ? '' : location.hash);
  constructor() {
    super();
    window.addEventListener('hashchange', () => this.sync());
  }
  sync(): void {
    this.route = parseHash(location.hash);
    this.dispatchEvent(new Event('change'));
  }
  go(...parts: (string | number)[]): void {
    location.hash = hashFor(...parts);
  }
}
