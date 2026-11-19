// Polls `bt/status` (1 s while a Bluetooth view is open; the add-on keeps the integration on its fast poll meanwhile).
import { ApiError, type Api, type BtStatus } from './api';

export class BtPoll extends EventTarget {
  status: BtStatus | null = null;
  error: ApiError | null = null;
  loaded = false;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private on = false;
  constructor(
    private api: Api,
    private hub: string,
    private every = 1000,
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
    try {
      this.status = await this.api.btStatus(this.hub);
      this.error = null;
    } catch (e) {
      this.error = e instanceof ApiError ? e : new ApiError(0, 'network', 'Cannot reach the add-on.');
    }
    this.loaded = true;
    this.dispatchEvent(new Event('change'));
    if (this.on) this.timer = setTimeout(() => void this.refresh(), typeof document !== 'undefined' && document.hidden ? this.every * 5 : this.every);
  }
}
