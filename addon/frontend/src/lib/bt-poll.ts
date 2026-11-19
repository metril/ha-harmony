// Polls `bt/status` (1 s while a Bluetooth view is open; the add-on keeps the integration on its fast poll meanwhile).
import { ApiError, type Api, type BtStatus } from './api';

export class BtPoll extends EventTarget {
  status: BtStatus | null = null;
  error: ApiError | null = null;
  loaded = false;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private on = false;
  private seq = 0;
  private low = 0;
  constructor(
    private api: Api,
    private hub: string,
    private every = 2000,
  ) {
    super();
  }
  start(): void {
    if (this.on) return;
    this.on = true;
    void this.refresh();
  }
  stop(): void {
    this.on = false;
    clearTimeout(this.timer);
  }
  /** Fetch now (after a command) and keep the cycle going. */
  async refresh(): Promise<void> {
    clearTimeout(this.timer);
    const mine = ++this.seq;
    let got: BtStatus | null = null;
    let err: ApiError | null = null;
    try {
      got = await this.api.btStatus(this.hub);
    } catch (e) {
      err = e instanceof ApiError ? e : new ApiError(0, 'network', 'Cannot reach the add-on.');
    }
    if (mine !== this.seq) return; // a newer fetch started meanwhile: it owns the result and the timer
    if (got) {
      // An older state (lower `gen`) is not allowed to replace a newer one; a second one in a row is taken as a hub that restarted.
      const g = got.state?.gen;
      const cur = this.status?.state?.gen;
      if (g !== undefined && cur !== undefined && g < cur && this.low < 1) this.low++;
      else {
        this.low = 0;
        this.status = got;
      }
      this.error = null;
    } else this.error = err;
    this.loaded = true;
    this.dispatchEvent(new Event('change'));
    clearTimeout(this.timer); // never more than one timer
    if (this.on) this.timer = setTimeout(() => void this.refresh(), typeof document !== 'undefined' && document.hidden ? this.every * 5 : this.every);
  }
}
