import { css, html, nothing } from 'lit';
import { trimBytes } from '../lib/codes';
import { customElement, property, state } from 'lit/decorators.js';
import '../components/hh-action-picker';
import '../components/hh-chain-mini';
import '../components/hh-dialog';
import '../components/hh-mini-remote';
import { StoreElement, confirmAction } from '../components/base';
import { problemList } from '../components/parts';
import { problemsFor } from '../lib/problems';
import {
  MAX_ACTIVITIES, MAX_DEVICES, MAX_STEPS, isRefused, ZONES, ZONE_KEYS, addActivity, addActivityDevice, addControlPage, addStartButton, addStep, applyHandler, chainOrder, deleteActivity, handlerOf,
  matchZone, moveChain, moveStep, removeActivityDevice, removeStep, renameActivity, setActivityInput, timeline, type TItem, type Zone,
} from '../lib/activity-edit';
import { art, deviceKind } from '../lib/device-art';
import { powerStatus } from '../lib/device-edit';
import { stockIconSvg } from '../lib/icon-shapes';
import { icon } from '../lib/icons';
import type { Device } from '../lib/model';
import { startReorder } from '../lib/reorder-drag';
import { hashFor } from '../lib/router';
import { setStyle } from '../lib/set-style';
import { stockIconByName } from '../lib/stock-icons';
import { StudioState, type StudioHost } from '../lib/studio-state';
import { sourceAction, describeAction, type Source } from '../lib/studio-model';
import { shared } from '../lib/styles';
import { displayCommand } from '../lib/key-names';
import { keyTitle } from '../lib/remote-layout';

const ZONE_NOTE: Record<Zone, string> = { volume: 'Vol +, Vol −, Mute', navigation: 'Arrows, OK, Menu, Exit', transport: 'Play, Pause, Stop, Rew, FF, Rec', channels: 'Ch +, Ch −, Prev' };
const ZONE_NAME: Record<Zone, string> = { volume: 'Volume', navigation: 'Navigation', transport: 'Transport', channels: 'Channels' };

/** Activities: a gallery, and an editor with the signal chain, the start/stop timeline and "who handles it". */
@customElement('hh-activities')
export class HhActivities extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      .head { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
      .gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 14px; }
      .acard { display: grid; gap: 12px; padding: 16px; border: 1px solid var(--line); border-radius: var(--r3); background: var(--sf); color: var(--tx); text-decoration: none; }
      .acard:hover { box-shadow: var(--sh); border-color: var(--acc); }
      .acard h2 { margin: 0; font-size: 16px; font-weight: 650; display: flex; gap: 8px; align-items: center; }
      .newcard { display: grid; place-items: center; align-content: center; gap: 8px; min-height: 160px; border: 2px dashed var(--line); border-radius: var(--r3); background: transparent; color: var(--tx2); font-weight: 600; cursor: pointer; }
      .newcard:hover { border-color: var(--acc); color: var(--tx); background: var(--sf2); }
      .newcard .plus { width: 44px; height: 44px; border-radius: 50%; background: var(--accs); color: var(--acc); display: grid; place-items: center; }
      /* editor */
      .tabs { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
      .tabs .tl { display: contents; }
      .tabs a { height: 32px; padding: 0 14px; border-radius: 16px; border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 600; font-size: 13px; text-decoration: none; display: inline-flex; align-items: center; }
      .tabs a[aria-selected="true"] { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
      .name { font-size: 20px; font-weight: 650; border: 1px solid transparent; border-radius: var(--r1); background: transparent; color: var(--tx); padding: 2px 8px; min-width: 0; flex: 1 1 200px; height: 40px; }
      .name:hover, .name:focus { border-color: var(--line); background: var(--sf); }
      section.card h2 { margin-bottom: 4px; }
      .sub { color: var(--tx3); font-size: 12.5px; margin: 0 0 12px; }
      .chain { display: flex; align-items: stretch; gap: 0; overflow-x: auto; padding: 6px 2px 10px; }
      .dev { position: relative; flex: none; width: 168px; display: grid; gap: 6px; justify-items: center; text-align: center; padding: 12px 10px 10px; border: 1.5px solid var(--line); border-radius: var(--r3); background: var(--sf); }
      .dev.over { border-color: var(--acc); box-shadow: 0 0 0 3px var(--accs); }
      .dev .art { width: 72px; height: 54px; color: var(--acc); cursor: grab; touch-action: none; }
      .dev b { font-size: 14px; max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .dev .chips { display: flex; gap: 4px; flex-wrap: wrap; justify-content: center; }
      .dev .tools { display: flex; gap: 2px; }
      .tools button, .stepx { width: 28px; height: 28px; border: 0; border-radius: 14px; background: transparent; color: var(--tx2); cursor: pointer; display: grid; place-items: center; padding: 0; font-size: 14px; }
      .tools button:hover:not(:disabled), .stepx:hover { background: var(--sf3); }
      .tools button:disabled { opacity: 0.3; cursor: default; }
      .tools .ic, .stepx .ic { width: 15px; height: 15px; }
      .wire { position: relative; flex: none; width: 96px; display: grid; align-content: center; justify-items: center; }
      .first { position: relative; min-height: 26px; display: grid; justify-items: center; }
      .first .pill { position: static; transform: none; }
      .first .menu { top: 100%; }
      .wire svg { width: 100%; height: 22px; stroke: var(--acc); fill: none; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }
      .pill { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -135%); max-width: 100%; height: 24px; padding: 0 10px; border-radius: 12px; border: 1.5px solid var(--acc); background: var(--sf); color: var(--tx); font-size: 11.5px; font-weight: 700; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
      .pill:hover { background: var(--accs); }
      .pill.none { border-style: dashed; color: var(--tx3); }
      .menu { position: absolute; z-index: 6; top: 58%; left: 50%; transform: translateX(-50%); min-width: 150px; padding: 4px; display: grid; gap: 1px; background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); box-shadow: var(--lift); }
      .menu button { text-align: left; height: 32px; padding: 0 10px; border: 0; border-radius: var(--r1); background: transparent; color: var(--tx); cursor: pointer; font-size: 13px; }
      .menu button:hover, .menu button[aria-checked="true"] { background: var(--accs); }
      .slot { flex: none; width: 168px; min-height: 140px; display: grid; place-items: center; align-content: center; gap: 6px; text-align: center; border: 2px dashed var(--line); border-radius: var(--r3); color: var(--tx3); font-weight: 600; margin-left: 8px; padding: 10px; }
      :host(.dragging) .slot, :host(.dragging) .zone { border-color: var(--acc); background: var(--accs); color: var(--tx); }
      .tray { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; margin-top: 4px; }
      .dchip { display: inline-flex; align-items: center; gap: 6px; height: 32px; padding: 0 12px; border-radius: 16px; border: 1px solid var(--line); background: var(--sf2); color: var(--tx); cursor: grab; font-size: 13px; font-weight: 600; touch-action: none; user-select: none; }
      .dchip[aria-pressed="true"] { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
      .dchip .art { width: 22px; height: 16px; }
      .seg { display: inline-flex; gap: 2px; padding: 2px; background: var(--sf2); border-radius: var(--r1); }
      .seg button { height: 30px; padding: 0 12px; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); font-weight: 600; font-size: 12px; cursor: pointer; }
      .seg button[aria-pressed="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.14); }
      .tl { display: flex; align-items: center; gap: 8px 6px; flex-wrap: wrap; padding: 8px 2px 12px; min-height: 64px; }
      .step { flex: none; display: inline-flex; align-items: center; gap: 4px; height: 40px; padding: 0 6px 0 12px; border-radius: 20px; font-size: 13px; font-weight: 600; max-width: 260px; }
      .step span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .step .tools { display: flex; }
      .step.auto { border: 1.5px dashed var(--line); background: var(--sf2); color: var(--tx2); padding-right: 12px; }
      .step.auto .sic { width: 14px; height: 14px; opacity: 0.7; }
      .step.user { border: 1.5px solid var(--acc); background: var(--sf); color: var(--tx); cursor: grab; touch-action: none; }
      .step.user.over { box-shadow: 0 0 0 3px var(--accs); }
      .step.user .ord { width: 20px; height: 20px; border-radius: 10px; background: var(--accs); font-size: 11px; display: grid; place-items: center; }
      .gap { flex: none; display: grid; justify-items: center; gap: 2px; font-size: 11px; color: var(--tx3); }
      .gap i { display: block; height: 0; border-top: 3px dotted var(--tx3); }
      .addstep { margin-top: 8px; display: grid; gap: 8px; padding: 12px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf2); max-width: 420px; }
      .who { display: grid; grid-template-columns: minmax(0, 1fr) 120px minmax(0, 1fr); gap: 14px; align-items: center; }
      .zones { display: grid; gap: 14px; }
      .zone { display: grid; gap: 6px; padding: 12px; border: 1.5px dashed var(--line); border-radius: var(--r2); background: var(--sf); min-width: 0; cursor: pointer; }
      .zone.picked { border-color: var(--acc); background: var(--accs); }
      .zone h3 { margin: 0; font-size: 14px; display: flex; gap: 8px; align-items: center; justify-content: space-between; }
      .igrp { display: inline-flex; gap: 6px; align-items: center; flex-wrap: wrap; padding-right: 10px; }
      .zsel { position: relative; }
      .zbtn { display: flex; align-items: center; gap: 8px; width: 100%; height: 34px; padding: 0 8px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); cursor: pointer; text-align: left; }
      .zbtn span:first-of-type { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .zbtn .art { width: 24px; height: 18px; flex: none; }
      .zbtn.nobody { color: var(--tx2); border-style: dashed; }
      .zmenu { top: 100%; left: 0; transform: none; width: 100%; }
      .wire.accepts, :host(.dragging-input) .wire, :host(.dragging-input) .first { outline: 2px dashed var(--acc); outline-offset: 2px; border-radius: var(--r1); }
      .dchip.on { background: var(--acc); color: var(--onacc); }
      .zone select { height: 32px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 6px; min-width: 0; width: 100%; }
      .zone .rep { font-size: 12px; color: var(--tx2); }
      .zone .rep b { color: var(--ok); font-weight: 600; } .zone .rep em { color: var(--ir); font-style: normal; }
      .remote { width: 120px; justify-self: center; } .np { padding: 6px 10px; } .mt { margin-top: 12px; }
      .offer { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
      .total { color: var(--tx2); font-size: 12.5px; margin-left: auto; }
      input.txt { height: 36px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 10px; width: 100%; }
      .checks { display: grid; gap: 4px; max-height: 220px; overflow: auto; }
      .checks label { display: flex; gap: 8px; align-items: center; font-weight: 500; min-height: 32px; }
      @media (max-width: 860px) {
        .who { grid-template-columns: minmax(0, 1fr); }
        .remote { order: -1; margin: 0 auto; width: 120px; }
        .chain { flex-direction: column; align-items: center; overflow-x: visible; }
        .wire { width: 100%; height: 64px; }
        .wire svg { transform: rotate(90deg); width: 64px; height: 22px; }
        .pill { transform: translate(-50%, -50%) translateX(54px); }
        .slot { margin: 8px 0 0; }
      }
    `,
  ];
  @property() selected = '';
  @state() private when: 'start' | 'stop' = 'start';
  @state() private menu: string | null = null;
  @state() private picked: string | null = null;
  /** an input chip picked up (click, then click a wire) */
  @state() private pickedInput: { device: string; input: string } | null = null;
  @state() private zoneMenu: Zone | null = null;
  @state() private hoverZone: Zone | null = null;
  @state() private adding = false;
  @state() private newOpen = false;
  @state() private newName = '';
  @state() private newDevs: string[] = [];
  @state() private offer: { id: string; button: boolean; page: boolean } | null = null;
  @state() private over: { kind: 'chain' | 'step'; i: number } | null = null;
  private ha = new StudioState(this.store);
  private host_ = { state: this.ha } as unknown as StudioHost;

  override connectedCallback(): void {
    super.connectedCallback();
    this.ha.addEventListener('change', this.onHa);
    void this.ha.loadCapabilities();
    void this.ha.loadHa();
    window.addEventListener('pointerdown', this.closeMenu);
  }
  override disconnectedCallback(): void {
    this.ha.removeEventListener('change', this.onHa);
    window.removeEventListener('pointerdown', this.closeMenu);
    super.disconnectedCallback();
  }
  private onHa = () => this.requestUpdate();
  private closeMenu = (e: Event) => {
    if (this.menu && !(e.composedPath() as HTMLElement[]).some((n) => n.classList?.contains('wire') || n.classList?.contains('dev'))) this.menu = null;
    if (this.zoneMenu && !(e.composedPath() as HTMLElement[]).some((n) => n.classList?.contains('zsel'))) this.zoneMenu = null;
  };
  private get cfg() {
    return this.store.config;
  }
  private dev(id: string): Device | undefined {
    return (this.cfg.devices ?? []).find((d) => d.id === id);
  }
  private say(msg: string, kind: 'info' | 'error' = 'info') {
    this.store.toast(msg, kind, kind === 'info');
  }

  // ------------------------------------------------------------ edits
  private create() {
    const name = this.newName.trim();
    if (!name) return;
    if ((this.cfg.activities ?? []).length >= MAX_ACTIVITIES) return this.say(`The hub holds at most ${MAX_ACTIVITIES} activities.`, 'error');
    const r = addActivity(this.cfg, name, this.newDevs.map((device) => ({ device })));
    this.store.replace(r.config);
    this.newOpen = false;
    this.newName = '';
    this.newDevs = [];
    this.offer = { id: r.id, button: true, page: true };
    location.hash = hashFor('activities', r.id);
  }
  private addDev(id: string, device: string) {
    const r = addActivityDevice(this.cfg, id, device);
    if (isRefused(r)) return this.say(r.refused, 'error');
    this.store.replace(r);
  }
  private async removeActivity(id: string) {
    const a = this.cfg.activities?.find((x) => x.id === id);
    const ok = await confirmAction(this, { title: `Delete ${a?.name ?? id}?`, body: 'Keys and buttons that start it are cleared, and its steps are lost. You can undo this.', ok: 'Delete activity', danger: true });
    if (!ok) return;
    this.store.replace(deleteActivity(this.cfg, id));
    this.say(`Deleted ${a?.name ?? id}`);
    location.hash = hashFor('activities');
  }
  private zoneApply(id: string, zone: Zone, device: string | null) {
    const r = applyHandler(this.cfg, id, zone, device);
    this.store.replace(r.config);
    const d = device ? this.dev(device) : undefined;
    const bits = Object.entries(r.matched).map(([k, c]) => `${keyTitle(k)} → ${c}`);
    this.say(device ? `${d?.name ?? device} handles ${ZONE_NAME[zone].toLowerCase()}${bits.length ? `: ${bits.join(', ')}` : '. No command names matched, so no key was filled'}${r.missing.length ? `. No match for ${r.missing.map(keyTitle).join(', ')}` : ''}` : `${ZONE_NAME[zone]} is not handled by a device any more`);
    this.picked = null;
  }

  // ------------------------------------------------------------ dragging device chips onto the slot / zones
  private menuKey(e: KeyboardEvent) {
    const items = [...(e.currentTarget as HTMLElement).querySelectorAll<HTMLElement>('button')];
    const i = items.indexOf(e.target as HTMLElement);
    if (e.key === 'Escape') {
      e.preventDefault();
      const owner = this.zoneMenu;
      this.zoneMenu = null;
      void this.updateComplete.then(() => this.renderRoot.querySelector<HTMLElement>(`[data-drop="zone:${owner}"] .zbtn`)?.focus());
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      items[(i + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length]?.focus();
    }
  }
  private inputDown(e: PointerEvent, device: string, input: string) {
    if (e.button !== 0) return;
    const x0 = e.clientX;
    const y0 = e.clientY;
    let moved = false;
    const move = (ev: PointerEvent) => {
      if (!moved && Math.hypot(ev.clientX - x0, ev.clientY - y0) > 6) {
        moved = true;
        this.classList.add('dragging');
        this.classList.add('dragging-input');
      }
    };
    const up = (ev: PointerEvent) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      this.classList.remove('dragging', 'dragging-input');
      if (!moved) {
        const same = this.pickedInput?.device === device && this.pickedInput.input === input;
        this.pickedInput = same ? null : { device, input };
        return;
      }
      const t = (this.renderRoot as ShadowRoot).elementFromPoint(ev.clientX, ev.clientY)?.closest<HTMLElement>('[data-drop^="wire:"]')?.dataset['drop'];
      if (t) this.setInput(device, input, t.slice(5));
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }
  /** An input chip belongs to one device: only that device's wire takes it. */
  private setInput(device: string, input: string, wireDevice: string) {
    this.pickedInput = null;
    if (device !== wireDevice) return this.say(`${input} is an input of ${this.dev(device)?.name ?? device}; drop it on the wire into that device.`, 'error');
    this.store.replace(setActivityInput(this.cfg, this.selected, device, input));
    this.say(`${this.dev(device)?.name ?? device} switches to ${input}`);
  }
  private chipDown(e: PointerEvent, device: string) {
    if (e.button !== 0) return;
    const x0 = e.clientX;
    const y0 = e.clientY;
    let moved = false;
    const move = (ev: PointerEvent) => {
      if (!moved && Math.hypot(ev.clientX - x0, ev.clientY - y0) > 6) {
        moved = true;
        this.classList.add('dragging');
      }
    };
    const up = (ev: PointerEvent) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      this.classList.remove('dragging');
      if (!moved) {
        this.picked = this.picked === device ? null : device;
        return;
      }
      const t = (this.renderRoot as ShadowRoot).elementFromPoint(ev.clientX, ev.clientY)?.closest<HTMLElement>('[data-drop]')?.dataset['drop'];
      if (t) this.drop(device, t);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }
  private drop(device: string, target: string) {
    const id = this.selected;
    if (target === 'slot') this.addDev(id, device);
    else if (target.startsWith('zone:')) this.zoneApply(id, target.slice(5) as Zone, device);
  }

  // ------------------------------------------------------------ gallery
  private gallery() {
    const acts = this.cfg.activities ?? [];
    return html`
      <div class="head"><h1>Activities</h1><span class="chip">${acts.length} / 16</span><span class="muted">What the remote does when you press a start button.</span></div>
      <div class="gallery">
        ${acts.map((a) => html`<a class="acard" href=${hashFor('activities', a.id)}><h2>${stockIconSvg(stockIconByName('play')?.id ?? 26)} ${a.name ?? a.id}</h2><hh-chain-mini .cfg=${this.cfg} activity=${a.id}></hh-chain-mini>
          <span class="muted">${(a.devices ?? []).length} devices · ${(a.steps ?? []).length + (a.off ?? []).length} own steps · ${Object.keys(a.keys ?? {}).length} keys</span></a>`)}
        <button class="newcard" @click=${() => (this.newOpen = true)}><span class="plus">${icon('plus')}</span>New activity</button>
      </div>
      ${this.newDialog()}`;
  }
  private newDialog() {
    const devs = this.cfg.devices ?? [];
    return html`<hh-dialog .open=${this.newOpen} heading="New activity" @close=${() => (this.newOpen = false)}>
      <label>Name<input class="txt" type="text" aria-label="Activity name" placeholder="Watch TV" .value=${this.newName} @input=${(e: Event) => (this.newName = (e.target as HTMLInputElement).value)} @keydown=${(e: KeyboardEvent) => e.key === 'Enter' && this.create()} /></label>
      <div><b>Devices it uses</b><p class="muted">You can change this later.</p>
        <div class="checks">${devs.length ? devs.map((d) => html`<label><input type="checkbox" .checked=${this.newDevs.includes(d.id)} @change=${(e: Event) => (this.newDevs = (e.target as HTMLInputElement).checked ? [...this.newDevs, d.id] : this.newDevs.filter((x) => x !== d.id))} /> ${d.name ?? d.id}</label>`) : html`<span class="muted">No devices yet. Add one first.</span>`}</div></div>
      <button slot="footer" class="btn" @click=${() => (this.newOpen = false)}>Cancel</button>
      <button slot="footer" class="btn pri" ?disabled=${!this.newName.trim()} @click=${() => this.create()}>Create</button>
    </hh-dialog>`;
  }

  // ------------------------------------------------------------ chain
  private inputPill(id: string, dv: string) {
    const d = this.dev(dv);
    const a = this.cfg.activities?.find((x) => x.id === id);
    const cur = a?.devices?.find((x) => x.device === dv)?.input;
    const names = Object.keys(d?.inputs ?? {});
    const open = this.menu === dv;
    return html`<button class="pill ${cur ? '' : 'none'}" aria-haspopup="menu" aria-expanded=${open} aria-label="Input on ${d?.name ?? dv}: ${cur ?? 'no change'}. Click to choose." @click=${() => { if (this.pickedInput) return this.setInput(this.pickedInput.device, this.pickedInput.input, dv); this.menu = open ? null : dv; }}>${cur ? displayCommand(cur) : names.length ? 'input?' : 'no inputs'}</button>
      ${open ? html`<div class="menu" role="menu" aria-label="Input for ${d?.name ?? dv}">
        <button role="menuitemradio" aria-checked=${!cur} @click=${() => { this.store.replace(setActivityInput(this.cfg, id, dv, null)); this.menu = null; }}>No input change</button>
        ${names.map((n) => html`<button role="menuitemradio" aria-checked=${cur === n} @click=${() => { this.store.replace(setActivityInput(this.cfg, id, dv, n)); this.menu = null; }}>${displayCommand(n)}</button>`)}
        ${names.length ? nothing : html`<span class="muted np">This device has no inputs</span>`}</div>` : nothing}`;
  }
  private card(id: string, dv: string, i: number, n: number) {
    const d = this.dev(dv);
    const ps = d ? powerStatus(d) : null;
    return html`<div class="dev ${this.over?.kind === 'chain' && this.over.i === i ? 'over' : ''}" data-chain=${i}>
      <span aria-hidden="true" title="Drag to reorder" @pointerdown=${(e: PointerEvent) => this.chainDown(e, id, i)}>${art(d ? deviceKind(d) : 'other', 'art')}</span>
      <b title=${d?.name ?? dv}>${d?.name ?? dv}</b>
      <div class="chips">${ps ? html`<span class="chip ${ps.tone === 'ok' ? 'ok' : ps.tone === 'ir' ? 'ir' : ''}">${ps.text.replace('Power ', '')}</span>` : nothing}${d?.settle_ms ? html`<span class="chip">${d.settle_ms / 1000} s settle</span>` : nothing}</div>
      ${i === 0 ? html`<div class="first" data-drop="wire:${dv}">${this.inputPill(id, dv)}</div>` : nothing}
      <div class="tools" role="group" aria-label="${d?.name ?? dv}">
        <button aria-label="Move ${d?.name ?? dv} earlier" ?disabled=${i === 0} @click=${() => this.store.replace(moveChain(this.cfg, id, i, i - 1))}>◀</button>
        <button aria-label="Move ${d?.name ?? dv} later" ?disabled=${i === n - 1} @click=${() => this.store.replace(moveChain(this.cfg, id, i, i + 1))}>▶</button>
        <button aria-label="Remove ${d?.name ?? dv} from this activity" @click=${() => { this.store.replace(removeActivityDevice(this.cfg, id, dv)); this.say(`Removed ${d?.name ?? dv}`); }}>${icon('x')}</button>
      </div></div>`;
  }
  private chainDown(e: PointerEvent, id: string, from: number) {
    startReorder(e, {
      from,
      items: () => [...this.renderRoot.querySelectorAll<HTMLElement>('.dev[data-chain]')],
      onOver: (i) => (this.over = i < 0 ? null : { kind: 'chain', i }),
      onDrop: (f, t) => {
        this.over = null;
        this.store.replace(moveChain(this.cfg, id, f, t));
      },
    });
  }
  private chain(id: string) {
    const order = chainOrder(this.cfg, id);
    const a = this.cfg.activities?.find((x) => x.id === id);
    const free = (this.cfg.devices ?? []).filter((d) => !(a?.devices ?? []).some((x) => x.device === d.id));
    return html`<section class="card" aria-label="Signal chain"><h2>Signal chain</h2>
      <p class="sub">Left to right, from the source to the screen. The pill on each wire is the input the next device switches to; click it to choose.</p>
      <div class="chain">
        ${order.map((dv, i) => html`${i > 0 ? html`<div class="wire ${this.pickedInput?.device === dv ? 'accepts' : ''}" data-drop="wire:${dv}"><svg viewBox="0 0 96 22" aria-hidden="true"><path d="M2 11H86M78 4l8 7-8 7"></path></svg>${this.inputPill(id, dv)}</div>` : nothing}${this.card(id, dv, i, order.length)}`)}
        ${order.length < MAX_DEVICES ? html`<div class="slot" data-drop="slot">${icon('plus')}<span>Drop a device</span><span class="muted">${order.length}/${MAX_DEVICES}</span></div>` : nothing}
      </div>
      ${order.some((dv) => Object.keys(this.dev(dv)?.inputs ?? {}).length)
        ? html`<div class="tray" role="group" aria-label="Inputs to drop on a wire"><span class="muted">Inputs: drag one onto its device's wire, or click it and then the wire.</span>${order.map((dv) => Object.keys(this.dev(dv)?.inputs ?? {}).length ? html`<span class="igrp"><span class="muted">${this.dev(dv)?.name ?? dv}</span>${Object.keys(this.dev(dv)?.inputs ?? {}).map((n) => html`<button class="dchip ${this.pickedInput?.device === dv && this.pickedInput.input === n ? 'on' : ''}" aria-pressed=${this.pickedInput?.device === dv && this.pickedInput.input === n} @pointerdown=${(e: PointerEvent) => this.inputDown(e, dv, n)} @click=${(e: MouseEvent) => e.preventDefault()} @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); this.setInput(dv, n, dv); } }} aria-label="Input ${n} of ${this.dev(dv)?.name ?? dv}. Press to switch to it.">${displayCommand(n)}</button>`)}</span>` : nothing)}</div>`
        : nothing}
      <div class="tray" role="group" aria-label="Devices to add">${free.length ? html`<span class="muted">Add:</span>${free.map((d) => html`<button class="dchip" @pointerdown=${(e: PointerEvent) => this.chipDown(e, d.id)} @click=${(e: MouseEvent) => e.preventDefault()} @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); this.addDev(id, d.id); } }} aria-label="Add ${d.name ?? d.id} to ${a?.name ?? id}. Click or drag to the slot.">${art(deviceKind(d), 'art')} ${d.name ?? d.id}</button>`)}` : html`<span class="muted">Every device is already in this activity.</span>`}</div>
    </section>`;
  }

  // ------------------------------------------------------------ timeline
  private step(id: string, it: Extract<TItem, { kind: 'user' }>, total: number) {
    const i = it.index;
    const move = (to: number) => this.store.replace(moveStep(this.cfg, id, this.when, i, to));
    return html`<span class="step user ${this.over?.kind === 'step' && this.over.i === i ? 'over' : ''}" data-step=${i} tabindex="0" role="group" aria-label="Step ${i + 1} of ${total}: ${it.label}. Alt plus arrows to move."
      @pointerdown=${(e: PointerEvent) => { if ((e.target as HTMLElement).closest('button')) return; startReorder(e, { from: i, items: () => [...this.renderRoot.querySelectorAll<HTMLElement>('.step[data-step]')], onOver: (t) => (this.over = t < 0 ? null : { kind: 'step', i: t }), onDrop: (f, t) => { this.over = null; this.store.replace(moveStep(this.cfg, id, this.when, f, t)); } }); }}
      @keydown=${(e: KeyboardEvent) => { if (e.altKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) { e.preventDefault(); const to = i + (e.key === 'ArrowLeft' ? -1 : 1); move(to); void this.updateComplete.then(() => this.renderRoot.querySelector<HTMLElement>(`.step[data-step="${Math.max(0, Math.min(total - 1, to))}"]`)?.focus()); } }}>
      <span class="ord">${i + 1}</span><span title=${it.label}>${it.label}</span>
      <span class="tools"><button aria-label="Move step ${i + 1} earlier" ?disabled=${i === 0} @click=${() => move(i - 1)}>◀</button><button aria-label="Move step ${i + 1} later" ?disabled=${i === total - 1} @click=${() => move(i + 1)}>▶</button></span>
      <button class="stepx" aria-label="Remove step ${i + 1}" @click=${() => this.store.replace(removeStep(this.cfg, id, this.when, i))}>${icon('x')}</button></span>`;
  }
  private timeline(id: string) {
    const t = timeline(this.cfg, id, this.when);
    const own = t.items.filter((x) => x.kind === 'user').length;
    const lock = stockIconByName('lock')?.id ?? 0;
    return html`<section class="card" aria-label="Sequence"><h2>Sequence</h2>
      <div class="head"><div class="seg" role="group" aria-label="Sequence"><button aria-pressed=${this.when === 'start'} @click=${() => (this.when = 'start')}>When it starts</button><button aria-pressed=${this.when === 'stop'} @click=${() => (this.when = 'stop')}>When it stops</button></div>
        <span class="total">${t.totalMs ? `${(t.totalMs / 1000).toFixed(1)} s of waiting · ` : ''}${own} / ${MAX_STEPS} own steps</span></div>
      <p class="sub">Grey steps are generated from the devices and cannot be edited. Your own steps run ${this.when === 'start' ? 'after them' : 'before the devices switch off'}; drag them, or use the arrows or Alt+←/→.</p>
      <div class="tl" role="list" aria-label="${this.when === 'start' ? 'Start' : 'Stop'} steps">
        ${t.items.map((it) =>
          it.kind === 'gap'
            ? html`<span class="gap" role="listitem" aria-label="Wait ${it.ms / 1000} seconds"><i style=${setStyle({ width: `${Math.max(28, Math.min(150, (it.ms / 1000) * 22))}px` })}></i>${it.ms / 1000} s</span>`
            : it.kind === 'auto'
              ? html`<span class="step auto" role="listitem" title="Generated from the device (locked)">${stockIconSvg(lock)}<span>${it.label}${it.ha ? ' (HA)' : ''}</span></span>`
              : html`<span role="listitem">${this.step(id, it, own)}</span>`,
        )}
        ${t.items.length === 0 ? html`<span class="muted">Nothing runs. Add devices above.</span>` : nothing}
      </div>
      ${this.adding
        ? html`<div class="addstep"><b>Add a step</b>
            <hh-action-picker .host=${this.host_} mode="widget" @pick=${(e: CustomEvent<Source>) => this.pickStep(id, e.detail)}></hh-action-picker>
            <button class="btn sm" @click=${() => (this.adding = false)}>Cancel</button></div>`
        : html`<div><button class="btn" ?disabled=${own >= MAX_STEPS} @click=${() => (this.adding = true)}>+ Add step</button></div>`}
    </section>`;
  }
  private pickStep(id: string, s: Source) {
    if (s.kind === 'none' || s.kind === 'default') return;
    const a = sourceAction(s);
    if (!a) return;
    const r = addStep(this.cfg, id, this.when, a);
    if (isRefused(r)) return this.say(r.refused, 'error');
    this.store.replace(r);
    this.adding = false;
    this.say(`Added step: ${describeAction(this.cfg, a)}`);
  }

  // ------------------------------------------------------------ who handles it
  private who(id: string) {
    const a = this.cfg.activities?.find((x) => x.id === id);
    const order = chainOrder(this.cfg, id);
    const marks: Record<string, string> = {};
    const zoneCards = ZONES.map((z) => {
      const dv = handlerOf(this.cfg, id, z);
      const d = dv ? this.dev(dv) : undefined;
      const m = d ? matchZone(d, z) : null;
      const kept = m ? Object.entries(m.matched).filter(([k, c]) => { const cur = a?.keys?.[k] as { device?: string; command?: string } | undefined; return !cur || cur.device !== dv || cur.command !== c; }).map(([k]) => k) : [];
      if (m) for (const k of ZONE_KEYS[z]) marks[k] = m.missing.includes(k) ? 'miss' : kept.includes(k) ? 'chg' : 'on';
      const sel = this.picked !== null;
      if (this.hoverZone === z) for (const k of ZONE_KEYS[z]) marks[k] = marks[k] ?? 'pulse';
      return html`<div class="zone ${sel ? 'picked' : ''}" data-drop="zone:${z}" @click=${(e: Event) => { if (this.picked && !(e.target as HTMLElement).closest('.zsel')) this.zoneApply(id, z, this.picked); }}
        @pointerenter=${() => (this.hoverZone = z)} @pointerleave=${() => (this.hoverZone = null)} @focusin=${() => (this.hoverZone = z)} @focusout=${() => (this.hoverZone = null)}>
        <h3>${ZONE_NAME[z]}</h3>
        <span class="muted">${ZONE_NOTE[z]}</span>
        <div class="zsel">
          <button class="zbtn ${dv ? '' : 'nobody'}" aria-haspopup="menu" aria-expanded=${this.zoneMenu === z} aria-label="Who handles ${ZONE_NAME[z].toLowerCase()}: ${d?.name ?? dv ?? 'nobody'}. Choose a device."
            @click=${(e: Event) => { e.stopPropagation(); this.zoneMenu = this.zoneMenu === z ? null : z; }}
            @keydown=${(e: KeyboardEvent) => { if (e.key === 'ArrowDown' && this.zoneMenu !== z) { e.preventDefault(); this.zoneMenu = z; } }}>
            ${d ? html`${art(deviceKind(d), 'art')}<span>${d.name ?? d.id}</span>` : html`<span>Nobody (keys stay free)</span>`}<span aria-hidden="true">▾</span></button>
          ${this.zoneMenu === z
            ? html`<div class="menu zmenu" role="menu" aria-label="Device for ${ZONE_NAME[z].toLowerCase()}" @keydown=${(e: KeyboardEvent) => this.menuKey(e)}>
                <button role="menuitemradio" aria-checked=${!dv} @click=${(e: Event) => { e.stopPropagation(); this.zoneApply(id, z, null); this.zoneMenu = null; }}>Nobody (keys stay free)</button>
                ${order.map((x) => html`<button role="menuitemradio" aria-checked=${x === dv} @click=${(e: Event) => { e.stopPropagation(); this.zoneApply(id, z, x); this.zoneMenu = null; }}>${this.dev(x)?.name ?? x}</button>`)}</div>`
            : nothing}
        </div>
        ${m ? html`<div class="rep">${Object.keys(m.matched).length ? html`<b>Filled:</b> ${Object.entries(m.matched).map(([k, c]) => `${keyTitle(k)} → ${c}`).join(', ')}.` : nothing}${kept.length ? html` <em>Kept your own for ${kept.map(keyTitle).join(', ')}.</em>` : nothing}
            ${m.missing.length ? html` <em>No match for ${m.missing.map(keyTitle).join(', ')}: ${m.missing.length === 1 ? 'it' : 'they'} stay free.</em> <a href=${hashFor('remote', id, m.missing[0] as string)}>Open on the remote</a>` : nothing}</div>` : html`<span class="rep">Pick a device, or drop one here.</span>`}
      </div>`;
    });
    return html`<section class="card" aria-label="Who handles it"><h2>Who handles it</h2>
      <p class="sub">Choose which device answers each group of keys. Keys are filled by matching command names; anything you set on a key yourself is kept.</p>
      <div class="tray" role="group" aria-label="Devices of this activity"><span class="muted">Drag a device onto a zone, or click one and then a zone:</span>${order.map((dv) => html`<button class="dchip" aria-pressed=${this.picked === dv} @pointerdown=${(e: PointerEvent) => this.chipDown(e, dv)} @click=${(e: MouseEvent) => e.preventDefault()} @keydown=${(e: KeyboardEvent) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); this.picked = this.picked === dv ? null : dv; } }}>${art(deviceKind(this.dev(dv) ?? { id: dv }), 'art')} ${this.dev(dv)?.name ?? dv}</button>`)}</div>
      <div class="who mt">
        <div class="zones">${zoneCards[0]}${zoneCards[1]}</div>
        <hh-mini-remote class="remote" .marks=${marks} label="Keys filled by who handles it"></hh-mini-remote>
        <div class="zones">${zoneCards[2]}${zoneCards[3]}</div>
      </div></section>`;
  }

  private editor(id: string) {
    const a = this.cfg.activities?.find((x) => x.id === id);
    if (!a) return html`<div class="card"><h2>That activity is gone</h2><a class="btn" href=${hashFor('activities')}>All activities</a></div>`;
    const o = this.offer?.id === id ? this.offer : null;
    return html`
      <div class="tabs"><div role="tablist" aria-label="Activities" class="tl">${(this.cfg.activities ?? []).map((x) => html`<a role="tab" href=${hashFor('activities', x.id)} aria-selected=${x.id === id}>${x.name ?? x.id}</a>`)}</div><button class="btn sm" @click=${() => (this.newOpen = true)}>+ New</button></div>
      <div class="head"><input class="name" aria-label="Activity name" .value=${a.name ?? a.id} @change=${(e: Event) => this.store.replace(renameActivity(this.cfg, id, trimBytes((e.target as HTMLInputElement).value.trim()) || id), `an${id}`)} />
        <a class="btn" href=${hashFor('remote', id)}>Open on remote</a><button class="btn danger" @click=${() => void this.removeActivity(id)}>Delete</button></div>
      ${o ? html`<div class="banner" role="status"><span><b>${a.name ?? id}</b> is new. Make it easy to start?</span><span class="sp"></span><span class="offer">
        ${o.button ? html`<button class="btn sm pri" @click=${() => this.offerStart(id)}>Add start button to Home</button>` : nothing}
        ${o.page ? html`<button class="btn sm pri" @click=${() => { this.store.replace(addControlPage(this.cfg, id)); this.say('Added a control page from the Activity control template'); this.offer = o.button ? { ...o, page: false } : null; }}>Add a control page</button>` : nothing}
        <button class="btn sm" @click=${() => (this.offer = null)}>Not now</button></span></div>` : nothing}
      ${problemList(problemsFor(this.store.errors, 'activities', (this.cfg.activities ?? []).findIndex((x) => x.id === id)))}
      ${this.chain(id)}${this.timeline(id)}${this.who(id)}${this.newDialog()}`;
  }
  private offerStart(id: string) {
    const r = addStartButton(this.cfg, id);
    if ('refused' in r) return this.say(r.refused, 'error');
    this.store.replace(r.config);
    this.say(r.message);
    this.offer = this.offer?.page ? { ...this.offer, button: false } : null;
  }

  protected override render() {
    return this.selected ? this.editor(this.selected) : this.gallery();
  }
}
