// One WebSocket to `api/v1/live` for the whole editor: topic subscriptions (refcounted), a snapshot cache, resync.
// No timers except the reconnect back-off; a visibility or online event reconnects at once. With no subscriber the socket is closed.
type Cb = (v: unknown) => void;
const MAX_TOPICS = 16;

export class Live extends EventTarget {
  /** the socket is open (data may still be stale until the first snapshot of a topic) */
  up = false;
  private ws: WebSocket | null = null;
  private subs = new Map<string, Set<Cb>>();
  private cache = new Map<string, unknown>();
  private delay = 1000;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private watching = false;

  constructor(private url: string | (() => string)) {
    super();
  }

  /** Subscribe to `topic` (`hubs`, `hub:<id>`, `bt:<id>`, `keys:<id>`); the cached snapshot, if any, is delivered at once. Returns the unsubscribe. */
  sub(topic: string, cb: Cb): () => void {
    let set = this.subs.get(topic);
    const first = !set;
    if (!set) this.subs.set(topic, (set = new Set()));
    set.add(cb);
    if (this.cache.has(topic)) cb(this.cache.get(topic));
    this.watch(true);
    if (first) {
      if (this.ws && this.up) this.sendSub();
      else this.connect();
    }
    return () => {
      const s = this.subs.get(topic);
      if (!s?.delete(cb) || s.size) return;
      this.subs.delete(topic);
      this.cache.delete(topic);
      if (!this.subs.size) this.shutdown();
      else if (this.up) this.sendSub();
    };
  }
  /** The last payload of `topic`, or undefined before its first snapshot. */
  get(topic: string): unknown {
    return this.cache.get(topic);
  }

  private watch(on: boolean) {
    if (on === this.watching || typeof document === 'undefined') return;
    this.watching = on;
    const f = on ? 'addEventListener' : 'removeEventListener';
    document[f]('visibilitychange', this.wake);
    window[f]('online', this.wake);
  }
  private wake = () => {
    if (typeof document !== 'undefined' && document.hidden) return;
    if (this.subs.size && !this.ws) this.connect();
  };
  private shutdown() {
    clearTimeout(this.timer);
    this.timer = undefined;
    this.watch(false);
    const ws = this.ws;
    this.ws = null;
    this.setUp(false);
    this.delay = 1000;
    ws?.close();
  }
  private setUp(v: boolean) {
    if (this.up === v) return;
    this.up = v;
    this.dispatchEvent(new Event('state'));
  }
  private sendSub() {
    this.ws?.send(JSON.stringify({ sub: [...this.subs.keys()].slice(0, MAX_TOPICS) }));
  }
  private connect() {
    if (this.ws || !this.subs.size || typeof WebSocket === 'undefined') return;
    clearTimeout(this.timer);
    this.timer = undefined;
    let ws: WebSocket;
    try {
      ws = new WebSocket(typeof this.url === 'function' ? this.url() : this.url);
    } catch {
      return this.retry();
    }
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.delay = 1000;
      this.setUp(true);
      this.sendSub();
    };
    ws.onmessage = (e) => {
      if (this.ws === ws) this.message(e.data);
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.setUp(false);
      this.retry();
    };
  }
  private retry() {
    if (!this.subs.size || this.timer) return;
    const wait = Math.max(1000, this.delay * (0.5 + Math.random() / 2));
    this.delay = Math.min(30000, this.delay * 2);
    this.timer = setTimeout(() => {
      this.timer = undefined;
      this.connect();
    }, wait);
  }

  private message(data: unknown) {
    let m: { t?: string; hub?: string; topic?: string; topics?: Record<string, unknown> } & Record<string, unknown>;
    try {
      m = JSON.parse(String(data));
    } catch {
      return;
    }
    const { t, ...rest } = m;
    switch (t) {
      case 'snapshot':
        for (const [k, v] of Object.entries(m.topics ?? {})) this.set(k, v);
        break;
      case 'hubs':
        this.set('hubs', rest);
        break;
      case 'hub':
      case 'bt':
        if (typeof m.hub === 'string') this.set(`${t}:${m.hub}`, rest);
        break;
      case 'key':
        if (typeof m.hub === 'string') this.emit(`keys:${m.hub}`, rest);
        break;
      case 'resync':
        if (typeof m.topic === 'string') this.cache.delete(m.topic); // a fresh snapshot follows
        break;
    }
  }
  private set(topic: string, v: unknown) {
    if (!this.subs.has(topic)) return;
    this.cache.set(topic, v);
    this.emit(topic, v);
  }
  private emit(topic: string, v: unknown) {
    for (const cb of [...(this.subs.get(topic) ?? [])]) cb(v);
  }
}
