// Shared base of the Bluetooth views: follows the pushed Bluetooth status, redraws a pairing countdown, loads the add-on's code tables.
import { css, html, nothing, type TemplateResult } from 'lit';
import { state } from 'lit/decorators.js';
import { StoreElement } from '../components/base';
import { statusChip } from '../components/parts';
import { ApiError, type BtCodes, type BtState, type BtStatus } from '../lib/api';
import { BT_DOWN, LINK_CHIP, bars, btError, secsLeft, type Countdown } from '../lib/bt-model';

export const btCss = css`
  :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; max-width: 1040px; width: 100%; margin: 0 auto; }
  h1 { font-size: 26px; margin: 0; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); margin: 6px 0 0; font-weight: 650; }
  h1:focus { outline: none; }
  .head { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
  .head .muted { font-size: 13px; }
  .top { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
  .steps { display: flex; gap: 14px; align-items: center; flex: 1; justify-content: center; flex-wrap: wrap; margin: 0; padding: 0; }
  .steps li { list-style: none; display: flex; align-items: center; gap: 6px; color: var(--tx3); font-size: 12px; font-weight: 600; }
  .steps li::before { content: ''; width: 10px; height: 10px; border-radius: 50%; background: var(--sf3); }
  .steps li.done::before { background: var(--acc); }
  .steps li.now { color: var(--tx); } .steps li.now::before { background: var(--acc); box-shadow: 0 0 0 4px var(--accs); }
  .lead { color: var(--tx2); margin: 0; }
  .center { text-align: center; justify-items: center; }
  .list { display: grid; gap: 10px; margin: 0; padding: 0; list-style: none; }
  .peer { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 10px 16px; align-items: center; padding: 14px 16px; }
  .peer .who { min-width: 0; display: grid; gap: 6px; }
  .peer .who b { font-size: 15px; font-weight: 650; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .meta { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }
  .meta .mono { font-size: 12px; color: var(--tx3); }
  .acts { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; justify-content: flex-end; }
  .acts select { height: 34px; max-width: 180px; }
  .tests { display: flex; gap: 6px; align-items: center; }
  .msg { font-size: 12.5px; color: var(--tx2); flex-basis: 100%; grid-column: 1 / -1; }
  .msg.bad { color: var(--bad); }
  select, input[type="text"], input[type="search"], input:not([type]) { height: 36px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; min-width: 0; }
  .f { display: grid; gap: 4px; font-size: 12px; font-weight: 600; color: var(--tx2); }
  .f input { width: 100%; }
  .hint { color: var(--tx3); font-size: 12.5px; font-weight: 400; margin: 0; }
  .err { color: var(--bad); font-size: 12.5px; font-weight: 500; }
  .stage { display: grid; gap: 16px; justify-items: center; text-align: center; padding: 16px 0; }
  .stage .btn { height: 46px; padding: 0 22px; font-size: 15px; }
  .timer { font-size: 44px; font-weight: 700; font-variant-numeric: tabular-nums; letter-spacing: 0.02em; color: var(--acc); }
  .steps-help { text-align: left; max-width: 560px; margin: 0; padding-left: 20px; color: var(--tx2); display: grid; gap: 4px; }
  .pulse { width: 14px; height: 14px; border-radius: 50%; background: var(--acc); display: inline-block; animation: hh-pulse 1.4s ease-in-out infinite; }
  @keyframes hh-pulse { 0%, 100% { opacity: 0.3; transform: scale(0.8); } 50% { opacity: 1; transform: scale(1.1); } }
  @media (prefers-reduced-motion: reduce) { .pulse { animation: none; } }
  .opts { display: grid; gap: 8px; text-align: left; width: min(520px, 100%); }
  .opt { display: grid; grid-template-columns: auto 1fr; gap: 2px 10px; align-items: start; padding: 10px 14px; border: 1.5px solid var(--line); border-radius: var(--r2); background: var(--sf); cursor: pointer; }
  .opt:has(input:checked), .opt[aria-pressed="true"] { border-color: var(--acc); background: var(--accs); }
  button.opt { grid-template-columns: 1fr; color: var(--tx); font: inherit; text-align: left; }
  .opt input { grid-row: span 2; margin-top: 3px; }
  .opt small { color: var(--tx3); font-size: 12.5px; }
  .cands { display: grid; gap: 8px; width: min(620px, 100%); margin: 0; padding: 0; list-style: none; text-align: left; }
  .cand { display: grid; grid-template-columns: minmax(0, 1fr) auto auto; gap: 12px; align-items: center; padding: 10px 14px; min-height: 56px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); }
  .cand b { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; display: block; }
  .cand .mono { font-size: 12px; color: var(--tx3); }
  .sig { display: inline-flex; gap: 2px; align-items: flex-end; height: 16px; }
  .sig i { width: 4px; background: var(--sf3); border-radius: 1px; }
  .sig i:nth-child(1) { height: 5px; } .sig i:nth-child(2) { height: 8px; } .sig i:nth-child(3) { height: 12px; } .sig i:nth-child(4) { height: 16px; }
  .sig i.on { background: var(--acc); }
  .form { display: grid; grid-template-columns: minmax(0, 1fr); gap: 14px; width: 100%; min-width: 0; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .pad { padding: 14px 16px; }
  .narrow { max-width: 520px; }
  label.f.narrow { max-width: 320px; }
  .big { font-size: 18px; color: var(--tx); }
  .bare { border: 0; padding: 0; margin: 0; }
  .side { display: grid; grid-template-columns: minmax(0, 1fr); gap: 12px; align-content: start; min-width: 0; }
  .twocol { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 16px; align-items: start; }
  @media (max-width: 860px) { .twocol { grid-template-columns: minmax(0, 1fr); } h1 { font-size: 22px; } .peer { grid-template-columns: minmax(0, 1fr); } .acts { justify-content: flex-start; } .cand { grid-template-columns: minmax(0, 1fr) auto; } .cand .btn { grid-column: 1 / -1; } .timer { font-size: 38px; } }
`;

export const signal = (rssi: number): TemplateResult => {
  const n = bars(rssi);
  return html`<span class="sig" role="img" aria-label=${`Signal ${['unknown', 'weak', 'fair', 'good', 'strong'][n]}`}>${[1, 2, 3, 4].map((i) => html`<i class=${i <= n ? 'on' : ''}></i>`)}</span>`;
};

/** The live Bluetooth status (pushed by the add-on), a countdown that redraws only while a pairing window is open, and the code tables. */
export class BtElement extends StoreElement {
  /** the `bt:<hub>` topic; null until its first snapshot */
  @state() protected status: BtStatus | null = null;
  @state() protected now = Date.now();
  @state() protected codes: BtCodes | null = null;
  @state() protected codesErr = '';
  /** the live socket is down (the status shown may be stale) */
  @state() protected offline = false;
  protected countdown: Countdown | null = null;
  private unsub?: () => void;
  private tickTimer?: ReturnType<typeof setTimeout>;
  private onConn = () => (this.offline = !this.store.api.live.up);
  /** Called after every status arrives. */
  protected onStatus(): void {}

  override connectedCallback(): void {
    super.connectedCallback();
    const live = this.store.api.live;
    live.addEventListener('state', this.onConn);
    this.unsub = live.sub(`bt:${this.store.hub}`, (v) => {
      this.status = v as BtStatus;
      this.onStatus();
    });
    void this.loadCodes();
  }
  override disconnectedCallback(): void {
    this.store.api.live.removeEventListener('state', this.onConn);
    this.unsub?.();
    clearTimeout(this.tickTimer);
    super.disconnectedCallback();
  }
  /** One timeout to the next whole second, and only while a countdown shows: no clock runs otherwise. */
  protected override updated(): void {
    clearTimeout(this.tickTimer);
    const c = this.countdown;
    if (c && secsLeft(c, Date.now()) > 0) this.tickTimer = setTimeout(() => (this.now = Date.now()), 1000 - ((Date.now() - c.at) % 1000));
  }
  protected async loadCodes(): Promise<void> {
    try {
      this.codes = await this.store.api.btCodes();
      this.codesErr = '';
    } catch (e) {
      this.codesErr = e instanceof ApiError ? btError(e) : 'Could not load the command list.';
    }
  }
  protected get st(): BtStatus | null {
    return this.status;
  }
  protected get bt(): BtState | null {
    return this.status?.state ?? null;
  }
  protected get hub(): string {
    return this.store.hub;
  }

  /** The hub's Bluetooth state as one chip. */
  protected linkChip(): TemplateResult | typeof nothing {
    const st = this.st;
    if (!st) return nothing;
    if (st.status !== 'ok') {
      const d = BT_DOWN[st.status];
      return statusChip(d.tone, d.label, { title: d.title });
    }
    const c = LINK_CHIP[st.state?.state ?? 'off'];
    return c ? statusChip(c.tone, c.label, { title: c.help, check: c.tone === 'ok' }) : nothing;
  }
  /** Why Bluetooth cannot be used right now, as a banner; nothing when it can. */
  protected downBanner(): TemplateResult | typeof nothing {
    const st = this.st;
    if (this.offline && !st) return html`<div class="banner bad" role="alert"><span><b>Cannot reach the add-on.</b> Reconnecting…</span></div>`;
    if (!st || st.status === 'ok') return nothing;
    const d = BT_DOWN[st.status];
    return html`<div class="banner ${st.status === 'unsupported' ? '' : 'ir'}" role="status" id="bt-down"><span><b>${d.title}.</b> ${d.body}</span></div>`;
  }
  /** Is the hub able to pair right now (status ok, adapter present and not given up)? */
  protected canPair(): string | null {
    const st = this.st;
    if (!st) return 'Checking Bluetooth…';
    if (st.status !== 'ok') return BT_DOWN[st.status].title;
    const s = st.state?.state;
    if (s === 'no_adapter' || s === 'disabled') return LINK_CHIP[s]?.help ?? 'Bluetooth is not available.';
    return null;
  }
}
