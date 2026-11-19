import { LitElement, css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { repeat } from 'lit/directives/repeat.js';
import { shared } from '../lib/styles';
import type { StudioHost } from '../lib/studio-state';
import { HA_DOMAINS } from '../lib/studio-state';
import { ACTION_DOMAINS, haProblem } from '../lib/ha-rules';
import { GROUPS, HA_ENTITY_ID, deviceChips, describeAction, isHaAction, fieldData, groupCommand, pretty, resolveIr, type Source } from '../lib/studio-model';
import './hh-send-ir';

type Kind = 'command' | 'input' | 'activity' | 'alloff' | 'ha';
const KINDS: [Kind, string][] = [['command', 'Command'], ['input', 'Input'], ['activity', 'Activity'], ['alloff', 'All off'], ['ha', 'Home Assistant action']];

/** Pick an action for a hard key or button: device command or input, start an activity, all off, or a Home Assistant service. */
@customElement('hh-action-picker')
export class HhActionPicker extends LitElement {
  static override styles = [
    shared,
    css`
      :host { display: grid; gap: 10px; min-width: 0; font-size: 13px; }
      .cur { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; padding: 8px 10px; border-radius: var(--r1); background: var(--sf2); }
      .cur b { font-weight: 600; }
      .cur b.nm { min-width: 0; max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .sp { flex: 1; }
      .seg { display: flex; flex-wrap: wrap; gap: 4px; }
      .seg button { height: 30px; padding: 0 10px; border-radius: 15px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); cursor: pointer; font-size: 12px; font-weight: 600; }
      .seg button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .seg button:disabled { opacity: 0.5; cursor: not-allowed; }
      label { display: grid; gap: 4px; color: var(--tx2); font-size: 12px; font-weight: 600; }
      select, input[type="text"], input[type="search"] { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--sf); color: var(--tx); padding: 0 8px; min-width: 0; width: 100%; }
      .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
      .sug { display: flex; gap: 6px; flex-wrap: wrap; }
      .sug button { max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; height: 28px; padding: 0 10px; border-radius: 14px; border: 1px solid var(--line); background: var(--sf); color: var(--tx); cursor: pointer; font-size: 12px; }
      .sug button:hover { background: var(--sf2); }
      hh-ha-browser { max-height: 340px; overflow: auto; }
      .why { color: var(--tx3); font-size: 12px; }
      .chk { display: flex; gap: 8px; align-items: center; font-weight: 500; color: var(--tx); }
    `,
  ];
  @property({ attribute: false }) host!: StudioHost;
  /** the action now in place (a config action, or null) */
  @property({ attribute: false }) value: unknown = null;
  @property() mode: 'key' | 'widget' = 'key';
  /** the key layer is an activity: offer "Use Default" */
  @property({ type: Boolean }) inherits = false;
  /** what the key does now when it is not set in this layer (inherited from Default) */
  @property() current = '';
  @state() private kind: Kind = 'command';
  @state() private device = '';
  @state() private name = '';
  @state() private activity = '';
  @state() private service = '';
  @state() private entity = '';
  @state() private repeatHeld = false;
  @state() private fields: Record<string, string> = {};
  @state() private q = '';
  @state() private browse = false;

  private get cfg() {
    return this.host.state.store.config;
  }
  override connectedCallback(): void {
    super.connectedCallback();
    this.host.state.addEventListener('change', this.onState);
    void this.host.state.loadHa();
  }
  override disconnectedCallback(): void {
    this.host.state.removeEventListener('change', this.onState);
    super.disconnectedCallback();
  }
  private onState = () => this.requestUpdate();
  private fire(source: Source) {
    this.dispatchEvent(new CustomEvent('pick', { detail: source, bubbles: true, composed: true }));
  }

  private suggestions(): { label: string; source: Source }[] {
    const q = this.q.trim().toLowerCase();
    if (!q) return [];
    const out: { label: string; source: Source }[] = [];
    for (const d of this.cfg.devices ?? []) {
      for (const c of Object.values(deviceChips(d)).flat())
        if (`${d.name ?? d.id} ${c.name}`.toLowerCase().includes(q)) out.push({ label: `${d.name ?? d.id} · ${c.name}`, source: { kind: c.kind, device: d.id, name: c.name } });
    }
    for (const a of this.cfg.activities ?? []) if (`${a.name ?? a.id}`.toLowerCase().includes(q)) out.push({ label: `Start ${a.name ?? a.id}`, source: { kind: 'activity', id: a.id } });
    for (const e of this.host.state.entities ?? []) if (`${e.name ?? ''} ${e.entity_id}`.toLowerCase().includes(q) && out.length < 40) out.push({ label: e.name ?? e.entity_id, source: { kind: 'entity', entity_id: e.entity_id, name: e.name } });
    return out.slice(0, 8);
  }

  private serviceOptions(): string[] {
    const sv = this.host.state.services;
    if (!sv) return [];
    return sv.filter((s) => ACTION_DOMAINS.includes(s.domain)).map((s) => `${s.domain}.${s.service}`);
  }

  private commit() {
    const k = this.kind;
    if (k === 'command' || k === 'input') {
      if (this.device && this.name) this.fire({ kind: k, device: this.device, name: this.name });
    } else if (k === 'activity') {
      if (this.activity) this.fire({ kind: 'activity', id: this.activity });
    } else if (k === 'alloff') this.fire({ kind: 'alloff' });
    else if (this.service && HA_ENTITY_ID.test(this.entity)) {
      const data = fieldData(this.fields);
      this.fire({ kind: 'ha', service: this.service, entity_id: this.entity, ...(data ? { data } : {}), ...(this.mode === 'key' && this.repeatHeld ? { repeat: true } : {}) });
    }
  }
  private ready(): boolean {
    const k = this.kind;
    if (k === 'command' || k === 'input') return !!(this.device && this.name);
    if (k === 'activity') return !!this.activity;
    if (k === 'alloff') return true;
    return this.host.state.haWritable && !!this.service && HA_ENTITY_ID.test(this.entity) && this.haIssue() === null;
  }
  private haIssue(): string | null {
    return haProblem(this.service, this.entity, fieldData(this.fields));
  }

  private deviceFields() {
    const devs = this.cfg.devices ?? [];
    const d = devs.find((x) => x.id === this.device);
    const names = !d ? [] : this.kind === 'input' ? Object.keys(d.inputs ?? {}) : Object.keys(d.commands ?? {});
    return html`
      <label>Device
        <select .value=${this.device} @change=${(e: Event) => { this.device = (e.target as HTMLSelectElement).value; this.name = ''; }}>
          <option value="">Choose a device</option>
          ${devs.map((x) => html`<option value=${x.id} ?selected=${x.id === this.device}>${x.name ?? x.id}</option>`)}
        </select>
      </label>
      <label>${this.kind === 'input' ? 'Input' : 'Command'}
        <select .value=${this.name} ?disabled=${!d} @change=${(e: Event) => (this.name = (e.target as HTMLSelectElement).value)}>
          <option value="">Choose…</option>
          ${this.kind === 'command' && d
            ? GROUPS.map((g) => {
                const list = [...(d.power ? ['power_on', 'power_off'] : []), ...names].filter((n) => groupCommand(n) === g || (g === 'power' && (n === 'power_on' || n === 'power_off')));
                return list.length ? html`<optgroup label=${pretty(g)}>${[...new Set(list)].map((n) => html`<option value=${n} ?selected=${n === this.name}>${n}</option>`)}</optgroup>` : nothing;
              })
            : names.map((n) => html`<option value=${n} ?selected=${n === this.name}>${n}</option>`)}
        </select>
      </label>`;
  }

  private serviceFields(): string[] {
    const [domain, service] = this.service.split('.');
    return (this.host.state.services ?? []).find((s) => s.domain === domain && s.service === service)?.fields ?? [];
  }
  private haFields() {
    const st = this.host.state;
    const services = this.serviceOptions();
    const dom = this.service.split('.')[0] ?? '';
    const domains = dom && HA_DOMAINS.includes(dom) ? [dom] : HA_DOMAINS;
    return html`
      ${st.haWritable ? nothing : html`<p class="why" role="note">Home Assistant actions need a newer add-on and integration (this one cannot store them yet).</p>`}
      <label>Service
        ${services.length
          ? html`<select .value=${this.service} @change=${(e: Event) => { this.service = (e.target as HTMLSelectElement).value; this.fields = {}; }}>
              <option value="">Choose a service</option>
              ${services.map((s) => html`<option value=${s} ?selected=${s === this.service}>${s}</option>`)}
            </select>`
          : html`<input type="text" placeholder="light.toggle" .value=${this.service} @input=${(e: Event) => (this.service = (e.target as HTMLInputElement).value.trim())} />`}
      </label>
      <label>Entity
        <input type="text" placeholder=${st.entities ? 'Search entities' : 'light.living_room'} .value=${this.entity} @input=${(e: Event) => (this.entity = (e.target as HTMLInputElement).value.trim())} />
      </label>
      ${st.entities ? html`<button class="btn sm" aria-expanded=${this.browse} @click=${() => { this.browse = !this.browse; void import('./hh-ha-browser'); }}>${this.browse ? 'Hide' : 'Browse'} Home Assistant entities</button>` : nothing}
      ${this.browse
        ? html`<hh-ha-browser mode="select" scope="picker" .entities=${st.entities} .loaded=${st.haLoaded} .loading=${st.haLoading} .reason=${st.haReason} .registries=${st.haRegistries} .areas=${st.haAreas} .domains=${domains}
            @hh-ha-pick=${(e: CustomEvent<{ entity_id: string }>) => { this.entity = e.detail.entity_id; this.browse = false; }}
            @hh-ha-retry=${() => void st.loadHa(true)}></hh-ha-browser>`
        : nothing}
      ${st.entities === null && st.haLoaded ? html`<p class="why">Home Assistant could not be read, so type the entity id.</p>` : nothing}
      ${this.entity && !HA_ENTITY_ID.test(this.entity) ? html`<p class="why" role="alert">Entity ids look like light.living_room.</p>` : this.haIssue() ? html`<p class="why" role="alert">${this.haIssue()}</p>` : nothing}
      ${this.serviceFields().map((f) => html`<label>${f}<input type="text" aria-label="Field ${f}" placeholder="optional" .value=${this.fields[f] ?? ''} @input=${(e: Event) => (this.fields = { ...this.fields, [f]: (e.target as HTMLInputElement).value })} /></label>`)}
      ${this.mode === 'key' ? html`<label class="chk"><input type="checkbox" .checked=${this.repeatHeld} @change=${(e: Event) => (this.repeatHeld = (e.target as HTMLInputElement).checked)} /> Repeat while held</label>` : nothing}`;
  }

  protected override render() {
    if (!this.host) return nothing;
    const sug = this.suggestions();
    const cur = this.value ? describeAction(this.cfg, this.value, this.host.state.entityNames()) : this.current || 'Nothing';
    const ir = resolveIr(this.cfg, this.value);
    const ha = !ir && this.value && isHaAction(this.cfg, this.value);
    return html`
      <div class="cur"><span class="muted">Now</span> <b class="nm" title=${cur}>${cur}</b>${ir ? html`<span class="sp"></span><hh-send-ir .code=${ir} name=${cur}></hh-send-ir>` : ha ? html`<span class="sp"></span><hh-send-ir ha></hh-send-ir>` : nothing}</div>
      <label>Search
        <input type="search" placeholder="vol_up, Watch TV, light…" .value=${this.q} @input=${(e: Event) => (this.q = (e.target as HTMLInputElement).value)} />
      </label>
      ${sug.length ? html`<div class="sug" role="list" aria-label="Matches">${repeat(sug, (s) => s.label, (s) => html`<button role="listitem" @click=${() => this.fire(s.source)}>${s.label}</button>`)}</div>` : nothing}
      <div class="seg" role="group" aria-label="Kind of action">
        ${KINDS.map(([k, label]) => html`<button aria-pressed=${this.kind === k} @click=${() => (this.kind = k)}>${label}</button>`)}
      </div>
      ${this.kind === 'command' || this.kind === 'input'
        ? this.deviceFields()
        : this.kind === 'activity'
          ? html`<label>Activity
              <select .value=${this.activity} @change=${(e: Event) => (this.activity = (e.target as HTMLSelectElement).value)}>
                <option value="">Choose an activity</option>
                ${(this.cfg.activities ?? []).map((a) => html`<option value=${a.id} ?selected=${a.id === this.activity}>${a.name ?? a.id}</option>`)}
              </select>
            </label>`
          : this.kind === 'alloff'
            ? html`<p class="why">Starts the built-in "all off": every device switches off.</p>`
            : this.haFields()}
      <div class="row">
        <button class="btn pri sm" ?disabled=${!this.ready()} @click=${() => this.commit()}>Set action</button>
        ${this.mode === 'key' && this.inherits ? html`<button class="btn sm" @click=${() => this.fire({ kind: 'default' })}>Use Default</button>` : nothing}
        <button class="btn sm" @click=${() => this.fire({ kind: 'none' })}>${this.mode === 'key' ? 'Do nothing' : 'No action'}</button>
      </div>
      ${this.kind === 'ha' && !this.host.state.haWritable ? html`<span class="why">Disabled: this add-on version has no Home Assistant actions.</span>` : nothing}
    `;
  }
}
