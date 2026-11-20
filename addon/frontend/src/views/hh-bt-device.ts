import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { BtElement, btCss } from './bt-base';
import './hh-bt-commands';
import './hh-bt-test';
import { statusChip } from '../components/parts';
import { BT_DOWN, patchHost, sameAddr } from '../lib/bt-model';
import { commandNames } from '../lib/bt';
import type { Device } from '../lib/model';
import { hashFor } from '../lib/router';
import { shared } from '../lib/styles';

const ADDR = /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/;

/** The page body of a `kind: bt` device inside Devices: its address and link type, a test-fire and its command set.
 *  No IR parts (no power, inputs or ports). Edits go straight into the draft. */
@customElement('hh-bt-device')
export class HhBtDevice extends BtElement {
  static override styles = [shared, btCss, css`:host { max-width: none; } .card { padding: 14px 16px; display: grid; gap: 12px; align-content: start; } a { color: var(--acc); font-weight: 600; font-size: 13px; }`];
  @property({ attribute: false }) device!: Device;
  @state() private addr: string | null = null;

  private cmdMap(): Record<string, string> {
    const d = this.device;
    return Object.fromEntries(commandNames(d).map((n) => [n, (d.commands?.[n] as unknown as string | undefined) ?? n]));
  }
  private patch(p: Parameters<typeof patchHost>[2]) {
    this.store.replace(patchHost(this.store.config, this.device.id, p));
  }
  protected override render() {
    const d = this.device;
    const s = this.bt;
    const live = s?.hosts.find((h) => sameAddr(h.addr, d.address));
    const typed = this.addr ?? d.address ?? '';
    const bad = typed && !ADDR.test(typed) ? 'An address looks like AA:BB:CC:DD:EE:FF.' : '';
    const free = (s?.hosts ?? []).filter((h) => !(this.store.config.devices ?? []).some((x) => x.id !== d.id && sameAddr(x.address, h.addr)));
    const st = this.st;
    return html`
      <section class="card" aria-label="Connection"><h2>Connection</h2>
        <div class="meta">
          ${st && st.status !== 'ok' ? statusChip(BT_DOWN[st.status].tone, BT_DOWN[st.status].label) : live ? statusChip(live.connected ? 'ok' : '', live.connected ? 'Connected' : 'Paired, not connected', { check: live.connected }) : st ? statusChip('ir', d.address ? 'Not paired with the hub' : 'Not paired yet') : nothing}
          <a href=${hashFor('bluetooth')}>Manage in Bluetooth</a></div>
        <label class="f">Address
          <input type="text" aria-label="Bluetooth address" aria-invalid=${bad ? 'true' : 'false'} placeholder="AA:BB:CC:DD:EE:FF" .value=${typed}
            @input=${(e: Event) => (this.addr = (e.target as HTMLInputElement).value.trim())}
            @change=${() => { if (!typed) this.patch({ address: undefined }); else if (!bad) this.patch({ address: typed.toUpperCase() }); this.addr = null; }} />
          ${bad ? html`<span class="err" role="alert">${bad}</span>` : html`<span class="hint">The hub matches the pairing to this device by its address. Leave it empty until it is paired.</span>`}</label>
        ${free.length ? html`<label class="f">Use a paired device<select aria-label="Pick a paired device" @change=${(e: Event) => { const v = (e.target as HTMLSelectElement).value; if (v) this.patch({ address: v }); (e.target as HTMLSelectElement).value = ''; }}><option value="">Choose…</option>${free.map((h) => html`<option value=${h.addr}>${h.name ?? 'Unnamed device'} · ${h.addr}</option>`)}</select></label>` : nothing}
        <label class="f">Link type
          <select aria-label="Link type" @change=${(e: Event) => this.patch({ transport: (e.target as HTMLSelectElement).value as 'auto' | 'classic' | 'le' })}>
            ${(['auto', 'classic', 'le'] as const).map((t) => html`<option value=${t} ?selected=${(d.transport ?? 'auto') === t}>${{ auto: 'Automatic', classic: 'Bluetooth BR/EDR', le: 'Bluetooth LE' }[t]}</option>`)}</select></label>
      </section>
      <section class="card" aria-label="Test a command">
        <hh-bt-test .api=${this.store.api} hub=${this.hub} .peer=${live?.slot ?? null} .codes=${this.codes} .commands=${this.cmdMap()} label=${d.name ?? d.id} ?disabled=${!live?.connected} reason=${live ? 'Not connected right now. Wake the device and make sure its Bluetooth is on.' : ''}></hh-bt-test>
        <span class="hint">${live ? 'Presses the command once on the paired device.' : 'Available once the device is paired with the hub.'}</span>
      </section>
      <section class="card" aria-label="Commands"><h2>Commands</h2>
        <hh-bt-commands .codes=${this.codes} .value=${this.cmdMap()} @change=${(e: CustomEvent<Record<string, string>>) => this.patch({ commands: e.detail })}></hh-bt-commands>
        ${this.codesErr ? html`<div class="banner bad" role="alert"><span>${this.codesErr}</span><span class="sp"></span><button class="btn sm" @click=${() => void this.loadCodes()}>Retry</button></div>` : nothing}
      </section>`;
  }
}
