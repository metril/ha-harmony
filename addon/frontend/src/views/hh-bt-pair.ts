import { html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { signal, BtElement, btCss } from './bt-base';
import './hh-bt-commands';
import './hh-bt-keys';
import { type KeyMap, CmdId, LINK_CHIP, addHostDevice, addRemote, patchHost, patchRemote, clock, outcome, remoteNameProblem, remotesOf, secsLeft, nextCountdown, sameAddr } from '../lib/bt-model';
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
  private focusKey = '';
  private defaultsApplied = false;
  private remoteAddr = ''; // the draft remote made the moment a remote paired (remote mode)
  private devId = ''; // the draft entry made the moment a device paired (host mode)

  override disconnectedCallback(): void {
    // Only leaving the page stops the window: a re-parent or re-render disconnects and reconnects in the same task.
    queueMicrotask(() => {
      if (!this.isConnected && this.step === 'pair' && this.run) this.closeWindow();
    });
    super.disconnectedCallback();
  }
  protected override willUpdate(): void {
    if (this.addr && this.step === 'start' && !this.paired) {
      const e = this.mode === 'host' ? this.bt?.hosts.find((h) => sameAddr(h.addr, this.addr)) : this.bt?.remotes.find((r) => sameAddr(r.addr, this.addr));
      if (e) this.setup({ addr: e.addr, ...(e.name ? { name: e.name } : {}), ...('transport' in e ? { transport: e.transport } : {}) });
    }
    if (this.step === 'setup' && !this.defaultsApplied && this.codes && this.mode === 'host' && !Object.keys(this.commands).length) {
      this.defaultsApplied = true;
      this.commands = this.defaultCommands();
      if (this.devId) queueMicrotask(() => this.store.replace(patchHost(this.store.config, this.devId, { commands: this.commands })));
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
    if (this.mode === 'host') this.persist(p);
    else this.persistRemote(p);
  }
  private defaultCommands(): Record<string, string> {
    return Object.fromEntries((this.codes?.builtins ?? []).filter((b) => b.default).map((b) => [b.name, b.name]));
  }
  /** A paired device goes into the draft at once (address and the transport it paired on), so it is never lost between the
   *  hub keeping its bond and the user finishing the form. The form then edits that entry. */
  private persist(p: { addr: string; name?: string; transport?: 'classic' | 'le' }) {
    const cfg = this.store.config;
    const dup = (cfg.devices ?? []).find((d) => d.kind === 'bt' && sameAddr(d.address, p.addr));
    if (dup) {
      this.devId = dup.id;
      return;
    }
    if (capProblem(cfg, 'device')) return;
    if (this.codes && !this.defaultsApplied) {
      this.defaultsApplied = true;
      this.commands = this.defaultCommands();
    }
    const r = addHostDevice(cfg, { name: this.name, address: p.addr, transport: p.transport ?? (this.transport === 'any' ? 'auto' : this.transport), commands: this.commands });
    this.store.replace(r.config);
    this.devId = r.id;
  }
  /** A paired remote goes into the draft at once too (name and address); saving then completes that entry with its keys. */
  private persistRemote(p: { addr: string; name?: string }) {
    const cfg = this.store.config;
    if (remotesOf(cfg).some((r) => sameAddr(r.address, p.addr))) return; // already listed: the banner says so, saving adds another
    this.store.replace(addRemote(cfg, { name: this.name, address: p.addr }));
    this.remoteAddr = p.addr;
  }
  private remoteIndex(): number {
    return this.remoteAddr ? remotesOf(this.store.config).findIndex((r) => sameAddr(r.address, this.remoteAddr)) : -1;
  }
  private async start() {
    this.err = '';
    this.busy = true;
    const baseline = this.bt; // what the hub held before this window: the pair answer may already list the new state
    try {
      const r = await this.store.api.btPair(this.hub, this.mode, this.mode === 'host' ? this.transport : 'le', 120, this.cStart.id('pair', { mode: this.mode, transport: this.mode === 'host' ? this.transport : 'le' }));
      this.cStart.settle();
      if (!this.isConnected) return void this.closeWindow(); // left while the request was in flight: the hub opened a window nobody watches
      this.run = startRun(this.mode, baseline ?? r.state, Date.now());
      this.countdown = nextCountdown(null, this.bt, Date.now()); // the pushed status with the open window may have come before this answer
      this.step = 'pair';
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
      const full = capProblem(cfg, 'command', Object.keys(this.commands).length) ?? (this.devId ? null : capProblem(cfg, 'device'));
      if (full) return void this.store.toast(full, 'error');
      let id = this.devId;
      if (id) this.store.replace(patchHost(cfg, id, { name, commands: this.commands }));
      else {
        const r = addHostDevice(cfg, { name, address: p.addr, transport: p.transport ?? (this.transport === 'any' ? 'auto' : this.transport), commands: this.commands });
        this.store.replace(r.config);
        id = r.id;
      }
      this.store.toast(`Added ${name} to the draft (${Object.keys(this.commands).length} commands). Nothing is applied yet.`, 'info', true);
      location.hash = hashFor('devices', id);
    } else {
      const at = this.remoteIndex();
      const keys = Object.keys(this.keys).length ? { keys: this.keys } : {};
      this.store.replace(at >= 0 ? patchRemote(cfg, at, { name, ...keys }) : addRemote(cfg, { name, address: p.addr, ...keys }));
      this.store.toast(`Added the remote ${name} to the draft. Nothing is applied yet.`, 'info', true);
      location.hash = hashFor('bluetooth');
    }
  }
  private nameProblem(): string {
    if (this.mode === 'remote') return remoteNameProblem(this.store.config, this.name, this.remoteIndex());
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
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Add a Bluetooth Device' : 'Add a Bluetooth Remote'}</h1>
      <p class="lead">${host ? 'Pair a TV, TV box or phone so the hub can act as its keyboard and remote.' : 'Pair a Bluetooth remote so its buttons control your activities through the hub.'}</p>
      ${this.downBanner()}
      ${block && this.st?.status === 'ok' ? html`<div class="banner bad" role="alert">${block}</div>` : nothing}
      ${s === 'starting' || s === 'off' ? html`<p class="hint">${LINK_CHIP[s]?.help}</p>` : nothing}
      <div class="stage">
        ${host && le
          ? html`<fieldset class="opts bare"><legend class="vh">Connection Type</legend>
              ${(['any', 'classic', 'le'] as const).map((t) => html`<label class="opt"><input type="radio" name="tr" .checked=${this.transport === t} @change=${() => (this.transport = t)} /><b>${{ any: 'Automatic (recommended)', classic: 'Bluetooth BR/EDR', le: 'Bluetooth LE' }[t]}</b><small>${{ any: 'Let the device choose. Works with almost everything.', classic: 'For older TVs, boxes and speakers.', le: 'For phones and newer devices that look for low-energy keyboards.' }[t]}</small></label>`)}</fieldset>`
          : nothing}
        ${host ? nothing : html`<p class="hint narrow">Put the remote in pairing mode first (usually hold its Home and Back buttons, or a pairing button, until the light blinks). Bluetooth LE remotes only.</p>`}
        <button class="btn pri" ?disabled=${!!block || this.busy} @click=${() => void this.start()}>${this.busy ? 'Opening…' : host ? 'Start Pairing' : 'Scan for Remotes'}</button>
        ${this.err ? html`<div class="banner bad" role="alert"><span>${this.err}</span></div>` : nothing}
      </div>`;
  }
  private pairStep() {
    const ph = this.phase();
    const host = this.mode === 'host';
    const left = secsLeft(this.countdown, this.now);
    const timer = this.countdown ? html`<div class="timer" role="timer" aria-live="off" aria-label="Time left">${clock(left)}</div>` : nothing;
    const stopBtn = html`<button class="btn" @click=${() => void this.stop()}>Stop Pairing</button>`;
    let body;
    if (ph.t === 'failed') {
      const o = outcome(ph.result, host ? 'device' : 'remote');
      body = html`<div class="banner ${o.tone === 'bad' ? 'bad' : 'ir'}" role="alert"><span><b>${o.title}.</b> ${o.body}</span></div><button class="btn pri" @click=${() => this.retry()}>Try Again</button>`;
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
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Pair the Device' : 'Pair the Remote'}</h1>
      <div class="stage">${body}${this.err ? html`<div class="banner bad" role="alert"><span>${this.err}</span></div>` : nothing}</div>`;
  }
  private setupStep() {
    const p = this.paired;
    if (!p) return html`<h1 id="step-title" tabindex="-1">Looking for It…</h1><p class="lead" role="status">Waiting for the hub to list it.</p>`;
    const host = this.mode === 'host';
    const prob = this.nameProblem();
    const bytes = byteCounter(this.name);
    const slot = (host ? this.bt?.hosts : this.bt?.remotes)?.find((e) => sameAddr(e.addr, p.addr))?.slot ?? null;
    const dup = host || this.remoteAddr ? undefined : remotesOf(this.store.config).find((r) => sameAddr(r.address, p.addr));
    return html`<h1 id="step-title" tabindex="-1">${host ? 'Name It and Choose Its Commands' : 'Choose What Its Buttons Do'}</h1>
      <p class="lead">${outcome('ok', host ? 'device' : 'remote', p.name).title}. <span class="mono">${p.addr}</span>${p.transport ? ` · ${p.transport === 'le' ? 'Bluetooth LE' : 'Bluetooth BR/EDR'}` : ''}</p>
      ${dup ? html`<div class="banner ir" role="status">This address is already in your config${'name' in dup && dup.name ? ` as ${dup.name}` : ''}. Adding it again creates a second entry.</div>` : nothing}
      <div class="twocol">
        <div class="form">
          ${host
            ? html`<hh-bt-commands .codes=${this.codes} .value=${this.commands} @change=${(e: CustomEvent<Record<string, string>>) => (this.commands = e.detail)}></hh-bt-commands>`
            : html`<hh-bt-keys .codes=${this.codes} .pages=${this.st?.limits?.pages ?? [7, 12]} .value=${this.keys} .peer=${slot} .api=${this.store.api} hub=${this.hub} @change=${(e: CustomEvent<KeyMap>) => (this.keys = e.detail)}></hh-bt-keys>`}
          ${this.codesErr ? html`<div class="banner bad" role="alert"><span>${this.codesErr}</span><span class="sp"></span><button class="btn sm" @click=${() => void this.loadCodes()}>Retry</button></div>` : nothing}
        </div>
        <aside class="side card pad" aria-label="Summary">
          <label class="f">${host ? 'Device Name' : 'Remote Name'}<input type="text" aria-label="Name" aria-invalid=${prob ? 'true' : 'false'} .value=${this.name} @input=${(e: Event) => (this.name = trimBytes((e.target as HTMLInputElement).value))} /></label>
          <span class="hint">${bytes.used} of ${bytes.max} bytes (the hub stores at most ${bytes.max})</span>
          ${prob ? html`<span class="err" role="alert">${prob}</span>` : nothing}
          <button class="btn pri" ?disabled=${!!prob || !this.codes} @click=${() => this.save()}>${host ? 'Add Device' : 'Add Remote'}</button>
          <span class="hint">${host ? 'The device is already in the draft.' : this.remoteAddr ? 'The remote is already in the draft.' : 'Adds to the draft only.'} Review and apply it from the bar at the top.</span>
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
