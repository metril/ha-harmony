import { html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { signal, BtElement, btCss } from './bt-base';
import './hh-bt-commands';
import './hh-bt-keys';
import { type KeyMap, CmdId, LINK_CHIP, addHostDevice, addRemote, clock, outcome, remoteNameProblem, remotesOf, secsLeft, nextCountdown, sameAddr, type Countdown } from '../lib/bt-model';
import { announce, derive, observe, startRun, type Phase, type Run } from '../lib/bt-wizard';
import { byteCounter, trimBytes } from '../lib/codes';
import { capProblem } from '../lib/device-edit';
import { hashFor } from '../lib/router';
import { btError } from '../lib/bt-model';
import { shared } from '../lib/styles';

type Step = 'start' | 'pair' | 'setup';
const STEPS: Record<string, string[]> = { host: ['Start', 'Pair', 'Name and commands'], remote: ['Start', 'Pair', 'Buttons'] };

/** The pairing wizard for a device (a TV box, a phone) or a remote: open a window, watch it, then set the new peer up and
 *  save it into the draft. `addr` skips pairing for a peer the hub already holds but the config does not list. */
@customElement('hh-bt-pair')
export class HhBtPair extends BtElement {
  static override styles = [shared, btCss];
  @property() mode: 'host' | 'remote' = 'host';
  @property() addr = '';
  @state() private step: Step = 'start';
  @state() private run: Run | null = null;
  @state() private transport: 'any' | 'classic' | 'le' = 'any';
  @state() private busy = false;
  @state() private err = '';
  @state() private paired: { addr: string; name?: string; transport?: 'classic' | 'le' } | null = null;
  @state() private name = '';
  @state() private commands: Record<string, string> = {};
  @state() private keys: KeyMap = {};
  @state() private picking = '';
  private cStart = new CmdId();
  private cStop = new CmdId();
  private cPick = new CmdId();
  private countdown: Countdown | null = null;
  private focusKey = '';
  private defaultsApplied = false;

  override disconnectedCallback(): void {
    if (this.step === 'pair' && this.run) this.closeWindow();
    super.disconnectedCallback();
  }
  protected override willUpdate(): void {
    if (this.addr && this.step === 'start' && !this.paired) {
      const e = this.mode === 'host' ? this.bt?.hosts.find((h) => sameAddr(h.addr, this.addr)) : this.bt?.remotes.find((r) => sameAddr(r.addr, this.addr));
      if (e) this.setup({ addr: e.addr, ...(e.name ? { name: e.name } : {}), ...('transport' in e ? { transport: e.transport } : {}) });
    }
    if (this.step === 'setup' && !this.defaultsApplied && this.codes && this.mode === 'host' && !Object.keys(this.commands).length) {
      this.defaultsApplied = true;
      this.commands = Object.fromEntries(this.codes.builtins.filter((b) => b.default).map((b) => [b.name, b.name]));
    }
  }
  protected override onStatus(): void {
    const s = this.bt;
    this.countdown = nextCountdown(this.countdown, s, Date.now());
    if (this.step !== 'pair' || !this.run) return;
    this.run = observe(this.run, s);
    const ph = derive(this.run, s, Date.now());
    if (ph.t === 'paired') {
      void this.store.api.btPairStop(this.hub).catch(() => undefined);
      this.setup(ph);
    }
  }
  protected override updated(): void {
    const key = `${this.step}:${this.step === 'pair' && this.run ? this.phase().t : ''}`;
    if (key !== this.focusKey) {
      this.focusKey = key;
      const h = this.renderRoot.querySelector<HTMLElement>('#step-title');
      if (h && this.step !== 'pair') h.focus();
      else if (h && ['failed', 'paired'].includes(this.phase().t)) h.focus();
    }
  }

  private phase(): Phase {
    return this.run ? derive(this.run, this.bt, this.now) : { t: 'opening' };
  }
  private setup(p: { addr: string; name?: string; transport?: 'classic' | 'le' }) {
    this.paired = p;
    this.name = trimBytes(p.name ?? (this.mode === 'host' ? 'Bluetooth device' : 'Bluetooth remote'));
    this.step = 'setup';
  }
  private async start() {
    this.err = '';
    this.busy = true;
    try {
      const r = await this.store.api.btPair(this.hub, this.mode, this.mode === 'host' ? this.transport : 'le', 120, this.cStart.id('pair', { mode: this.mode, transport: this.mode === 'host' ? this.transport : 'le' }));
      this.cStart.settle();
      if (!this.isConnected) return void this.closeWindow(); // left while the request was in flight: the hub opened a window nobody watches
      this.run = startRun(this.mode, r.state ?? this.bt, Date.now());
      this.countdown = null;
      this.step = 'pair';
      void this.poll.refresh();
    } catch (e) {
      this.cStart.settle(e);
      if (!this.isConnected) return void this.closeWindow(); // a timeout may still have opened it
      this.err = btError(e);
    } finally {
      this.busy = false;
    }
  }
  private closeWindow() {
    void this.store.api.btPairStop(this.hub).catch(() => undefined);
  }
  private async stop() {
    try {
      await this.store.api.btPairStop(this.hub, this.cStop.id());
      this.cStop.settle();
    } catch (e) {
      this.cStop.settle(e);
      this.err = btError(e);
    }
    this.run = null;
    this.step = 'start';
    void this.poll.refresh();
  }
  private async pick(addr: string) {
    if (!this.run) return;
    this.picking = addr;
    this.err = '';
    try {
      await this.store.api.btPick(this.hub, addr, this.cPick.id('pick', { addr }));
      this.cPick.settle();
      if (!this.isConnected) return void this.closeWindow(); // left while pairing was requested: do not leave the scan window open
      this.run = { ...this.run, picked: addr };
    } catch (e) {
      this.cPick.settle(e);
      if (!this.isConnected) return void this.closeWindow();
      this.err = btError(e);
    } finally {
      this.picking = '';
      void this.poll.refresh();
    }
  }
  private retry() {
    this.run = null;
    this.err = '';
    this.step = 'start';
  }

  // ------------------------------------------------------------ saving
  private save() {
    const p = this.paired;
    if (!p) return;
    const cfg = this.store.config;
    const name = trimBytes(this.name.trim());
    if (this.mode === 'host') {
      const full = capProblem(cfg, 'device') ?? capProblem(cfg, 'command', Object.keys(this.commands).length);
      if (full) return void this.store.toast(full, 'error');
      const r = addHostDevice(cfg, { name, address: p.addr, transport: this.transport === 'any' ? 'auto' : this.transport, commands: this.commands });
      this.store.replace(r.config);
      this.store.toast(`Added ${name} to the draft (${Object.keys(this.commands).length} commands). Nothing is applied yet.`, 'info', true);
      location.hash = hashFor('devices', r.id);
    } else {
      this.store.replace(addRemote(cfg, { name, address: p.addr, ...(Object.keys(this.keys).length ? { keys: this.keys } : {}) }));
      this.store.toast(`Added the remote ${name} to the draft. Nothing is applied yet.`, 'info', true);
      location.hash = hashFor('bluetooth');
    }
  }
  private nameProblem(): string {
    if (this.mode === 'remote') return remoteNameProblem(this.store.config, this.name);
    const n = this.name.trim();
    if (!n) return 'Give it a name.';
    return byteCounter(n).over > 0 ? 'That name is too long.' : '';
  }

  // ------------------------------------------------------------ steps
  private startStep() {
    const host = this.mode === 'host';
    const block = this.canPair();
    const le = this.st?.limits?.le ?? false;
    const s = this.bt?.state;
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Add a Bluetooth device' : 'Add a Bluetooth remote'}</h1>
      <p class="lead">${host ? 'Pair a TV, TV box or phone so the hub can act as its keyboard and remote.' : 'Pair a Bluetooth remote so its buttons control your activities through the hub.'}</p>
      ${this.downBanner()}
      ${block && this.st?.status === 'ok' ? html`<div class="banner bad" role="alert">${block}</div>` : nothing}
      ${s === 'starting' || s === 'off' ? html`<p class="hint">${LINK_CHIP[s]?.help}</p>` : nothing}
      <div class="stage">
        ${host && le
          ? html`<fieldset class="opts bare"><legend class="vh">Connection type</legend>
              ${(['any', 'classic', 'le'] as const).map((t) => html`<label class="opt"><input type="radio" name="tr" .checked=${this.transport === t} @change=${() => (this.transport = t)} /><b>${{ any: 'Any (recommended)', classic: 'Classic Bluetooth', le: 'Bluetooth LE' }[t]}</b><small>${{ any: 'Let the device choose. Works with almost everything.', classic: 'For older TVs, boxes and speakers.', le: 'For phones and newer devices that look for low-energy keyboards.' }[t]}</small></label>`)}</fieldset>`
          : nothing}
        ${host ? nothing : html`<p class="hint narrow">Put the remote in pairing mode first (usually hold its Home and Back buttons, or a pairing button, until the light blinks). Bluetooth LE remotes only.</p>`}
        <button class="btn pri" ?disabled=${!!block || this.busy} @click=${() => void this.start()}>${this.busy ? 'Opening…' : host ? 'Start pairing' : 'Scan for remotes'}</button>
        ${this.err ? html`<div class="banner bad" role="alert"><span>${this.err}</span></div>` : nothing}
      </div>`;
  }
  private pairStep() {
    const ph = this.phase();
    const host = this.mode === 'host';
    const left = secsLeft(this.countdown, this.now);
    const timer = this.countdown ? html`<div class="timer" role="timer" aria-live="off" aria-label="Time left">${clock(left)}</div>` : nothing;
    const stopBtn = html`<button class="btn" @click=${() => void this.stop()}>Stop pairing</button>`;
    let body;
    if (ph.t === 'failed') {
      const o = outcome(ph.result, host ? 'device' : 'remote');
      body = html`<div class="banner ${o.tone === 'bad' ? 'bad' : 'ir'}" role="alert"><span><b>${o.title}.</b> ${o.body}</span></div><button class="btn pri" @click=${() => this.retry()}>Try again</button>`;
    } else if (ph.t === 'scanning') {
      body = html`${timer}
        <p class="lead">Put the remote in pairing mode (usually hold Home and Back until its light blinks). It appears below.</p>
        ${ph.candidates.length
          ? html`<ul class="cands" aria-label="Remotes found">${ph.candidates.map((c) => html`<li class="cand"><span><b>${c.name ?? 'Unnamed remote'}</b><span class="mono">${c.addr}</span></span>${signal(c.rssi)}<button class="btn sm pri" ?disabled=${!!this.picking} aria-label="Pair with ${c.name ?? c.addr}" @click=${() => void this.pick(c.addr)}>${this.picking === c.addr ? 'Pairing…' : 'Pair'}</button></li>`)}</ul>`
          : html`<span class="pulse" aria-hidden="true"></span><p class="hint">Looking for remotes in pairing mode…</p>`}
        ${stopBtn}`;
    } else if (ph.t === 'bonding') {
      body = html`<span class="pulse" aria-hidden="true"></span><p class="lead">Pairing with ${this.bt?.candidates?.find((c) => c.addr === ph.addr)?.name ?? ph.addr}…</p>${timer}${stopBtn}`;
    } else if (ph.t === 'waiting') {
      body = html`${timer}
        <p class="lead big">On your TV box, open Bluetooth settings and pair with “Harmony …”.</p>
        <ol class="steps-help"><li>Open <b>Bluetooth settings</b> on the device and let it search.</li><li>Choose the entry whose name starts with <b>Harmony</b>.</li><li>Accept the pairing request on the device, and here if it asks.</li></ol>
        <p class="hint">Not listed? Remove an old "Harmony" entry from the device's Bluetooth list first, and stay within a few metres of the hub.</p>
        ${stopBtn}`;
    } else body = html`<span class="pulse" aria-hidden="true"></span><p class="lead">Opening the pairing window…</p><p class="hint">Home Assistant relays this to the hub, which can take a few seconds.</p>`;
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Pair the device' : 'Pair the remote'}</h1>
      <div class="stage">${body}${this.err ? html`<div class="banner bad" role="alert"><span>${this.err}</span></div>` : nothing}</div>`;
  }
  private setupStep() {
    const p = this.paired;
    if (!p) return html`<h1 id="step-title" tabindex="-1">Looking for it…</h1><p class="lead" role="status">Waiting for the hub to list it.</p>`;
    const host = this.mode === 'host';
    const prob = this.nameProblem();
    const bytes = byteCounter(this.name);
    const slot = (host ? this.bt?.hosts : this.bt?.remotes)?.find((e) => sameAddr(e.addr, p.addr))?.slot ?? null;
    const dup = host ? (this.store.config.devices ?? []).find((d) => d.kind === 'bt' && sameAddr(d.address, p.addr)) : remotesOf(this.store.config).find((r) => sameAddr(r.address, p.addr));
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Name it and choose its commands' : 'Choose what its buttons do'}</h1>
      <p class="lead">${outcome('ok', host ? 'device' : 'remote', p.name).title}. <span class="mono">${p.addr}</span>${p.transport ? ` · ${p.transport === 'le' ? 'Bluetooth LE' : 'Classic'}` : ''}</p>
      ${dup ? html`<div class="banner ir" role="status">This address is already in your config${'name' in dup && dup.name ? ` as ${dup.name}` : ''}. Adding it again creates a second entry.</div>` : nothing}
      <div class="twocol">
        <div class="form">
          ${host
            ? html`<hh-bt-commands .codes=${this.codes} .value=${this.commands} @change=${(e: CustomEvent<Record<string, string>>) => (this.commands = e.detail)}></hh-bt-commands>`
            : html`<hh-bt-keys .codes=${this.codes} .pages=${this.st?.limits?.pages ?? [7, 12]} .value=${this.keys} .peer=${slot} .api=${this.store.api} hub=${this.hub} @change=${(e: CustomEvent<KeyMap>) => (this.keys = e.detail)}></hh-bt-keys>`}
          ${this.codesErr ? html`<div class="banner bad" role="alert"><span>${this.codesErr}</span><span class="sp"></span><button class="btn sm" @click=${() => void this.loadCodes()}>Retry</button></div>` : nothing}
        </div>
        <aside class="side card pad" aria-label="Summary">
          <label class="f">${host ? 'Device name' : 'Remote name'}<input type="text" aria-label="Name" aria-invalid=${prob ? 'true' : 'false'} .value=${this.name} @input=${(e: Event) => (this.name = trimBytes((e.target as HTMLInputElement).value))} /></label>
          <span class="hint">${bytes.used} of ${bytes.max} bytes (the hub stores at most ${bytes.max})</span>
          ${prob ? html`<span class="err" role="alert">${prob}</span>` : nothing}
          <button class="btn pri" ?disabled=${!!prob || !this.codes} @click=${() => this.save()}>${host ? 'Add device' : 'Add remote'}</button>
          <span class="hint">Adds to the draft only. Review and apply it from the bar at the top.</span>
        </aside>
      </div>`;
  }

  protected override render() {
    const steps = STEPS[this.mode] as string[];
    const at = this.step === 'start' && !this.addr ? 0 : this.step === 'pair' ? 1 : 2;
    const ph = this.step === 'pair' && this.run ? announce(this.phase(), this.mode) : '';
    return html`
      <div class="top">
        <a class="btn ghost" href=${hashFor('bluetooth')}>← Bluetooth</a>
        <ol class="steps" aria-label="Progress">${steps.map((s, i) => html`<li class=${i < at ? 'done' : i === at ? 'now' : ''} aria-current=${i === at ? 'step' : 'false'}>${s}</li>`)}</ol>
        <a class="btn ghost" href=${hashFor('bluetooth')}>Cancel</a>
      </div>
      <p class="vh" role="status" aria-live="polite">${ph}</p>
      ${this.step === 'start' && !this.addr ? this.startStep() : this.step === 'pair' ? this.pairStep() : this.setupStep()}`;
  }
}
