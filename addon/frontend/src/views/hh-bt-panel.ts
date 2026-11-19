import { html, nothing } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { BtElement, btCss } from './bt-base';
import './hh-bt-test';
import { confirmAction } from '../components/base';
import { statusChip } from '../components/parts';
import type { BtHost, BtRemote } from '../lib/api';
import { CmdId, LINK_CHIP, btDevices, btError, clock, limitText, nextCountdown, remotesOf, sameAddr, secsLeft, type Countdown } from '../lib/bt-model';
import { commandNames } from '../lib/bt';
import { hashFor } from '../lib/router';
import { icon } from '../lib/icons';
import { shared } from '../lib/styles';

/** The Bluetooth panel: the hub's Bluetooth state, what is paired, test-fire and forget, and the way into the pairing wizards. */
@customElement('hh-bt-panel')
export class HhBtPanel extends BtElement {
  static override styles = [shared, btCss];
  @state() private busy = '';
  @state() private notes: Record<string, string> = {};
  private countdown: Countdown | null = null;
  private cForget = new CmdId();

  protected override onStatus(): void {
    this.countdown = nextCountdown(this.countdown, this.bt, Date.now());
  }
  private async forget(kind: 'host' | 'remote', e: BtHost | BtRemote, label: string) {
    const yes = await confirmAction(this, {
      title: `Forget ${label}?`,
      body: `The hub removes its Bluetooth pairing with ${label}. ${kind === 'host' ? 'Its entry in your config stays; pair it again to use it.' : 'Its buttons stop working until you pair it again.'}`,
      ok: 'Forget',
      danger: true,
    });
    if (!yes) return;
    this.busy = e.addr;
    try {
      await this.store.api.btForget(this.hub, e.slot, kind, this.cForget.id());
      this.cForget.settle();
      this.store.toast(`Forgot ${label}.`);
      this.notes = { ...this.notes, [e.addr]: '' };
    } catch (err) {
      this.cForget.settle(err);
      this.notes = { ...this.notes, [e.addr]: btError(err) };
    } finally {
      this.busy = '';
      void this.poll.refresh();
    }
  }
  private async stopWindow() {
    try {
      await this.store.api.btPairStop(this.hub);
    } catch (err) {
      this.store.toast(btError(err), 'error');
    }
    void this.poll.refresh();
  }

  private hostRow(h: BtHost) {
    const cfg = btDevices(this.store.config).find((d) => sameAddr(d.address, h.addr));
    const label = cfg?.name ?? h.name ?? 'Unnamed device';
    const known = this.codes ? Object.fromEntries(this.codes.builtins.filter((b) => b.default).map((b) => [b.name, b.name])) : {};
    const cmds = cfg ? Object.fromEntries(commandNames(cfg).map((n) => [n, (cfg.commands?.[n] as unknown as string | undefined) ?? n])) : known;
    return html`<li class="card peer" aria-label=${label}>
      <div class="who"><b title=${label}>${label}</b>
        <div class="meta">
          ${statusChip(h.connected ? 'ok' : '', h.connected ? 'Connected' : 'Not connected', { check: h.connected })}
          <span class="chip">${h.transport === 'le' ? 'Bluetooth LE' : 'Classic'}</span>
          ${cfg ? html`<a class="chip acc" href=${hashFor('devices', cfg.id)} aria-label="Open ${label} in Devices">In your config</a>` : statusChip('ir', 'Not in your config')}
          <span class="mono">${h.addr}</span></div></div>
      <div class="acts">
        <hh-bt-test .api=${this.store.api} hub=${this.hub} .peer=${h.slot} .codes=${this.codes} .commands=${cmds} label=${label}></hh-bt-test>
        ${cfg ? nothing : html`<a class="btn sm pri" href=${hashFor('bluetooth', 'host', h.addr)}>Add to config</a>`}
        <button class="btn sm danger" ?disabled=${this.busy === h.addr} aria-label="Forget ${label}" @click=${() => void this.forget('host', h, label)}>Forget</button>
      </div>
      ${this.notes[h.addr] ? html`<span class="msg bad" role="alert">${this.notes[h.addr]}</span>` : nothing}
    </li>`;
  }
  private remoteRow(r: BtRemote) {
    const list = remotesOf(this.store.config);
    const i = list.findIndex((x) => sameAddr(x.address, r.addr));
    const label = list[i]?.name ?? r.name ?? 'Unnamed remote';
    return html`<li class="card peer" aria-label=${label}>
      <div class="who"><b title=${label}>${label}</b>
        <div class="meta">
          ${statusChip(r.connected ? 'ok' : '', r.connected ? 'Connected' : 'Not connected', { check: r.connected })}
          ${r.battery !== undefined ? html`<span class="chip ${r.battery < 15 ? 'bad' : ''}" title="Battery level">Battery ${r.battery}%</span>` : nothing}
          ${i >= 0 ? html`<a class="chip acc" href=${hashFor('bluetooth', 'keys', i)} aria-label="Edit the buttons of ${label}">In your config</a>` : statusChip('ir', 'Not in your config')}
          <span class="mono">${r.addr}</span></div></div>
      <div class="acts">
        ${i >= 0 ? html`<a class="btn sm" href=${hashFor('bluetooth', 'keys', i)}>Edit buttons</a>` : html`<a class="btn sm pri" href=${hashFor('bluetooth', 'remote', r.addr)}>Add to config</a>`}
        <button class="btn sm danger" ?disabled=${this.busy === r.addr} aria-label="Forget ${label}" @click=${() => void this.forget('remote', r, label)}>Forget</button>
      </div>
      ${this.notes[r.addr] ? html`<span class="msg bad" role="alert">${this.notes[r.addr]}</span>` : nothing}
    </li>`;
  }

  protected override render() {
    const st = this.st;
    const s = this.bt;
    const ok = st?.status === 'ok';
    const link = s ? LINK_CHIP[s.state] : undefined;
    const cfgHosts = btDevices(this.store.config).filter((d) => !s?.hosts.some((h) => sameAddr(h.addr, d.address)));
    const cfgRemotes = remotesOf(this.store.config).map((r, i) => ({ r, i })).filter(({ r }) => !s?.remotes.some((x) => sameAddr(x.addr, r.address)));
    const pairBlock = this.canPair();
    const left = secsLeft(this.countdown, this.now);
    const add = (what: 'host' | 'remote', cls: string, text: string) =>
      pairBlock ? html`<button class="btn" disabled title=${pairBlock}>${icon('plus')} ${text}</button>` : html`<a class="btn ${cls}" href=${hashFor('bluetooth', what)}>${icon('plus')} ${text}</a>`;
    const adds = (cls: string) => html`${add('host', cls, 'Add Bluetooth device')}${add('remote', '', 'Add Bluetooth remote')}`;
    return html`
      <div class="head"><h1>Bluetooth</h1>${this.linkChip()}${ok ? html`<span class="muted">${limitText(st)}</span>` : nothing}<span class="sp"></span>${ok ? adds('pri') : nothing}</div>
      ${!st && !this.poll.error ? html`<p class="muted" role="status">Checking Bluetooth…</p>` : nothing}
      ${this.downBanner()}
      ${ok && link && s && s.state !== 'ready' ? html`<div class="banner ${link.tone === 'bad' ? 'bad' : 'ir'}" role="status"><span>${link.help}</span></div>` : nothing}
      ${ok && s?.window
        ? html`<div class="banner" role="status"><span><b>A pairing window for a ${s.window === 'host' ? 'device' : 'remote'} is open${s.discoverable_secs !== undefined ? ` · ${clock(left)} left` : ''}.</b></span><span class="sp"></span>
            <a class="btn sm" href=${hashFor('bluetooth', s.window)}>Open</a><button class="btn sm" @click=${() => void this.stopWindow()}>Stop</button></div>`
        : nothing}
      ${ok && s
        ? html`
          <h2>Devices</h2>
          ${s.hosts.length ? html`<ul class="list" aria-label="Paired devices">${s.hosts.map((h) => this.hostRow(h))}</ul>` : html`<p class="muted">No device is paired yet. Add a TV box or phone to let the hub type to it.</p>`}
          ${s.remotes.length || (st?.limits?.remotes_max ?? 0) > 0 ? html`<h2>Remotes</h2>
            ${s.remotes.length ? html`<ul class="list" aria-label="Paired remotes">${s.remotes.map((r) => this.remoteRow(r))}</ul>` : html`<p class="muted">No remote is paired yet.</p>`}` : nothing}
          ${cfgHosts.length || cfgRemotes.length
            ? html`<h2>In your config, not paired</h2>
              <ul class="list" aria-label="Not paired">
                ${cfgHosts.map((d) => html`<li class="card peer"><div class="who"><b>${d.name ?? d.id}</b><div class="meta">${statusChip('ir', 'Not paired')}<span class="mono">${d.address ?? 'no address yet'}</span></div></div><div class="acts"><a class="btn sm" href=${hashFor('devices', d.id)}>Open</a></div></li>`)}
                ${cfgRemotes.map(({ r, i }) => html`<li class="card peer"><div class="who"><b>${r.name}</b><div class="meta">${statusChip('ir', 'Not paired')}<span class="mono">${r.address ?? 'no address yet'}</span></div></div><div class="acts"><a class="btn sm" href=${hashFor('bluetooth', 'keys', i)}>Edit buttons</a></div></li>`)}
              </ul>
              <p class="hint">Pair them with the buttons above; the hub matches them to these entries by address.</p>`
            : nothing}`
        : nothing}`;
  }
}
