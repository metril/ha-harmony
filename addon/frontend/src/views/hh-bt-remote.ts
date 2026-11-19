import { html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { BtElement, btCss } from './bt-base';
import './hh-bt-keys';
import { confirmAction } from '../components/base';
import { patchRemote, remoteNameProblem, remotesOf, removeRemote, sameAddr, type KeyMap } from '../lib/bt-model';
import { byteCounter, trimBytes } from '../lib/codes';
import { hashFor } from '../lib/router';
import { shared } from '../lib/styles';

/** Edit the name and key map of one configured Bluetooth remote (`#/bluetooth/keys/<index>`); edits go straight into the draft. */
@customElement('hh-bt-remote')
export class HhBtRemote extends BtElement {
  static override styles = [shared, btCss];
  @property({ type: Number }) index = 0;

  @state() private nameErr = '';
  private rename(input: HTMLInputElement) {
    const v = trimBytes(input.value.trim());
    this.nameErr = remoteNameProblem(this.store.config, v, this.index);
    if (this.nameErr) return;
    this.store.replace(patchRemote(this.store.config, this.index, { name: v }));
  }
  private async drop(name: string) {
    const yes = await confirmAction(this, { title: `Remove ${name} from the config?`, body: 'The remote stays paired with the hub, but its buttons stop doing anything until you add it again. You can undo this.', ok: 'Remove', danger: true });
    if (!yes) return;
    this.store.replace(removeRemote(this.store.config, this.index));
    location.hash = hashFor('bluetooth');
  }
  protected override render() {
    const r = remotesOf(this.store.config)[this.index];
    if (!r) return html`<div class="card pad"><h1>That remote is gone</h1><a class="btn" href=${hashFor('bluetooth')}>Back to Bluetooth</a></div>`;
    const live = this.bt?.remotes.find((x) => sameAddr(x.addr, r.address));
    const bytes = byteCounter(r.name);
    return html`
      <div class="top"><a class="btn ghost" href=${hashFor('bluetooth')}>← Bluetooth</a><span class="sp"></span><button class="btn danger" @click=${() => void this.drop(r.name)}>Remove from config</button></div>
      <div class="head"><h1>${r.name}</h1>${live ? html`<span class="chip ${live.connected ? 'ok' : ''}">${live.connected ? 'Connected' : 'Not connected'}</span>` : html`<span class="chip ir">Not paired</span>`}<span class="mono muted">${r.address ?? 'no address yet'}</span></div>
      ${this.downBanner()}
      <label class="f narrow">Name<input type="text" aria-label="Remote name" .value=${r.name} aria-invalid=${this.nameErr ? 'true' : 'false'} @change=${(e: Event) => this.rename(e.target as HTMLInputElement)} /></label>
      ${this.nameErr ? html`<span class="err" role="alert">${this.nameErr}</span>` : nothing}
      <span class="hint">${bytes.used} of ${bytes.max} bytes (the remote shows at most ${bytes.max})</span>
      <hh-bt-keys .codes=${this.codes} .pages=${this.st?.limits?.pages ?? [7, 12]} .value=${r.keys ?? {}} .peer=${live?.slot ?? null} .api=${this.store.api} hub=${this.hub}
        @change=${(e: CustomEvent<KeyMap>) => this.store.replace(patchRemote(this.store.config, this.index, { keys: e.detail }))}></hh-bt-keys>
      ${this.codesErr ? html`<div class="banner bad" role="alert"><span>${this.codesErr}</span><span class="sp"></span><button class="btn sm" @click=${() => void this.loadCodes()}>Retry</button></div>` : nothing}`;
  }
}
