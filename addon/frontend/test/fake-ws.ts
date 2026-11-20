// A fake WebSocket for the live socket: records every frame, answers `sub` with snapshots of `fakeLive.topics`, lets a test push frames.
export class FakeWS {
  static instances: FakeWS[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((e: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  sent: { sub?: string[] }[] = [];
  closed = false;
  constructor(public url: string) {
    FakeWS.instances.push(this);
    queueMicrotask(() => !this.closed && fakeLive.auto && this.onopen?.());
  }
  send(d: string) {
    const m = JSON.parse(d) as { sub?: string[] };
    this.sent.push(m);
    if (m.sub) {
      const topics = Object.fromEntries(m.sub.filter((t) => t in fakeLive.topics).map((t) => [t, fakeLive.topics[t]]));
      queueMicrotask(() => !this.closed && this.onmessage?.({ data: JSON.stringify({ t: 'snapshot', topics }) }));
    }
  }
  close() {
    this.closed = true;
  }
}
export const fakeLive = {
  /** topic -> snapshot payload, delivered after a `sub` for it */
  topics: {} as Record<string, unknown>,
  /** sockets open by themselves */
  auto: true,
  get sockets() {
    return FakeWS.instances;
  },
  get open() {
    return FakeWS.instances.filter((w) => !w.closed);
  },
  /** a server frame to every open socket */
  push(m: object) {
    for (const w of this.open) w.onmessage?.({ data: JSON.stringify(m) });
  },
  /** the server end drops every socket */
  drop() {
    for (const w of this.open) {
      w.closed = true;
      w.onclose?.();
    }
  },
  reset() {
    FakeWS.instances = [];
    this.topics = {};
    this.auto = true;
  },
};
