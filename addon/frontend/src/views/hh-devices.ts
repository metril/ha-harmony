import '../components/hh-icon-picker';
import { css, html, nothing, type TemplateResult } from 'lit';
import { displayCommand, dn } from '../lib/key-names';
import { orderBy, orderUnits } from '../lib/key-order';
import { customElement, property, state } from 'lit/decorators.js';
import '../components/hh-action-picker';
import '../components/hh-code-dialog';
import '../components/hh-send-ir';
import { StoreElement, confirmAction } from '../components/base';
import type { CodeEdit, CodeSave } from '../components/hh-code-dialog';
import { summarize, trimBytes } from '../lib/codes';
import { KIND_LABEL, commandIconId, deviceArt, deviceKind, groupIconId } from '../lib/device-art';
import { capProblem, commandCount, duplicateCommand, isHaDevice, mapCodes, nameProblem, patchDevice, powerMode, powerStatus, setCommand, setInput, setPowerCode, setPowerMode, sourceLabel, withPorts, withRepeats, type PowerMode } from '../lib/device-edit';
import { stockIconSvg } from '../lib/icon-shapes';
import { icon } from '../lib/icons';
import { LIMITS, type Device, type IrAction } from '../lib/model';
import { deleteDevice, deleteItem, describeRef, findRefs, refHref, renameItem, type Ref } from '../lib/refs';
import { hashFor } from '../lib/router';
import { shared } from '../lib/styles';
import { StudioState, type StudioHost } from '../lib/studio-state';
import { GROUPS, pretty, sourceAction, groupCommand, type Group, type Source } from '../lib/studio-model';
import { byteCounter } from '../lib/codes';
import { problemList } from '../components/parts';
import { problemsFor, type Problem } from '../lib/problems';

const HOST = (state: StudioState): StudioHost => ({ state }) as unknown as StudioHost;

/** Devices: a gallery of cards, and a keypad view of one device's commands (or a compact list for bulk edits). */
@customElement('hh-devices')
export class HhDevices extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
      .head { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
      .head h1 { flex: 0 1 auto; }
      .gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); gap: 14px; }
      .dcard { display: grid; grid-template-rows: auto 1fr; border: 1px solid var(--line); border-radius: var(--r3); background: var(--sf); color: var(--tx); text-decoration: none; overflow: hidden; transition: box-shadow 140ms ease-out, transform 140ms ease-out; }
      .dcard:hover { box-shadow: var(--sh); transform: translateY(-1px); }
      .dcard .top { display: grid; place-items: center; height: 104px; background: var(--sf2); color: var(--acc); position: relative; }
      details.iconpick { position: relative; }
      details.iconpick > summary { display: inline-flex; align-items: center; gap: 6px; cursor: pointer; list-style: none; padding: 2px 10px 2px 6px; border: 1px solid var(--line); border-radius: 16px; background: var(--sf2); font-size: 12.5px; }
      details.iconpick > summary::-webkit-details-marker { display: none; }
      details.iconpick .dic { width: 22px; height: 22px; color: var(--acc); }
      details.iconpick[open] > hh-icon-picker { position: absolute; z-index: 5; top: calc(100% + 6px); left: 0; width: 320px; max-width: 80vw; box-sizing: border-box; padding: 10px; border: 1px solid var(--line); border-radius: var(--r2); background: var(--sf); box-shadow: 0 6px 20px rgba(0, 0, 0, 0.18); }
      .dcard .top .art { width: 88px; height: 66px; }
      .dcard .errdot { position: absolute; top: 10px; right: 10px; }
      .dcard .body { padding: 12px 14px 14px; display: grid; gap: 8px; align-content: start; }
      .dcard h2 { margin: 0; font-size: 15px; font-weight: 650; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .dcard .src { font-size: 12px; color: var(--tx3); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .chips { display: flex; flex-wrap: wrap; gap: 6px; }
      .addw { display: grid; }
      .used summary { cursor: pointer; font-weight: 600; }
      .used ul { margin: 6px 0 0; padding: 0; list-style: none; display: grid; gap: 2px; max-height: 220px; overflow: auto; }
      .used li a { display: block; padding: 4px 6px; border-radius: var(--r1); color: var(--tx); text-decoration: none; font-size: 13px; }
      .used li a:hover { background: var(--sf2); text-decoration: underline; }
      .add { text-align: center; padding: 16px; display: grid; place-items: center; align-content: center; gap: 8px; min-height: 230px; border: 2px dashed var(--line); border-radius: var(--r3); color: var(--tx2); text-decoration: none; font-weight: 600; background: transparent; }
      .add:hover { background: var(--sf2); color: var(--tx); border-color: var(--acc); }
      .add .plus { width: 48px; height: 48px; border-radius: 50%; background: var(--accs); color: var(--acc); display: grid; place-items: center; }
      .add .ic { width: 24px; height: 24px; }
      .empty { text-align: center; padding: 40px 16px; display: grid; gap: 10px; justify-items: center; }
      /* device page */
      .dbar { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
      .back { display: inline-flex; align-items: center; gap: 4px; color: var(--tx2); text-decoration: none; font-weight: 600; height: 36px; }
      .name { font-size: 20px; font-weight: 650; border: 1px solid transparent; border-radius: var(--r1); background: transparent; color: var(--tx); padding: 2px 8px; min-width: 0; flex: 1 1 200px; height: 40px; }
      .name:hover, .name:focus { border-color: var(--line); background: var(--sf); }
      .seg { display: inline-flex; gap: 2px; padding: 2px; background: var(--sf2); border-radius: var(--r1); }
      .seg button { height: 30px; padding: 0 12px; border: 0; border-radius: 5px; background: transparent; color: var(--tx2); font-weight: 600; font-size: 12px; cursor: pointer; }
      .seg button[aria-pressed="true"] { background: var(--sf); color: var(--tx); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.14); }
      .page { display: grid; grid-template-columns: minmax(0, 1fr) 320px; gap: 16px; align-items: start; }
      .side { display: grid; gap: 14px; }
      .grp { margin-bottom: 18px; }
      .grp h2 { margin: 0 0 8px; font-size: 12px; letter-spacing: 0.04em; text-transform: uppercase; color: var(--tx3); font-weight: 650; display: flex; gap: 8px; align-items: center; }
      .grp h2 .sic { width: 18px; height: 18px; color: var(--acc); }
      .keys > .pair { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; grid-column: span 2; }
      @media (max-width: 480px) { .keys > .pair { grid-column: 1 / -1; } }
      .keys { display: grid; grid-template-columns: repeat(auto-fill, minmax(168px, 1fr)); gap: 8px; }
      .key { display: flex; align-items: center; gap: 6px; height: 48px; padding: 0 6px 0 4px; border-radius: var(--r2); border: 1px solid var(--line); background: linear-gradient(var(--sf), var(--sf2)); min-width: 0; }
      .key.bad { border-color: var(--bad); background: var(--bads); }
      .key .k { flex: 1; min-width: 0; height: 40px; display: flex; flex-direction: column; justify-content: center; gap: 0; border: 0; background: transparent; color: var(--tx); cursor: pointer; text-align: left; padding: 0 6px; border-radius: 8px; }
      .key .k:hover { background: var(--sf3); }
      .key .k b { font-weight: 600; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; display: flex; gap: 6px; align-items: center; }
      .key .k b .sic { width: 16px; height: 16px; color: var(--tx3); flex: none; }
      .key .k small { color: var(--tx3); font-size: 11px; font-family: var(--mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .key.ph { border-style: dashed; background: transparent; }
      .key.ph .k { color: var(--tx3); font-weight: 600; }
      .dpad { display: grid; grid-template-columns: repeat(3, 168px); gap: 8px; margin-bottom: 8px; max-width: 100%; overflow-x: auto; }
      .numpad { display: grid; grid-template-columns: repeat(3, minmax(120px, 168px)); gap: 8px; margin-bottom: 8px; }
      .row { display: flex; gap: 8px; align-items: center; justify-content: space-between; min-height: 44px; padding: 6px 0; border-bottom: 1px solid var(--line); }
      .row:last-child { border-bottom: 0; }
      .row .l { min-width: 0; display: grid; }
      .row .l b { font-weight: 600; font-size: 13px; }
      .row .l small { color: var(--tx3); font-family: var(--mono); font-size: 11.5px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .f { display: grid; gap: 4px; font-size: 12px; font-weight: 600; color: var(--tx2); }
      input[type="number"], input[type="text"], input[type="search"], select { height: 34px; border: 1px solid var(--line); border-radius: var(--r1); background: var(--bg); color: var(--tx); padding: 0 8px; min-width: 0; width: 100%; }
      .ports { display: flex; gap: 6px; }
      .ports button { width: 44px; height: 34px; border-radius: var(--r1); border: 1px solid var(--line); background: var(--sf); color: var(--tx2); font-weight: 700; cursor: pointer; }
      .ports button[aria-pressed="true"] { background: var(--accs); border-color: var(--acc); color: var(--tx); }
      .hint { color: var(--tx3); font-size: 12px; font-weight: 400; }
      .cont { display: contents; } .mt { margin-top: 10px; } .row.bare { border: 0; padding: 0; } .side.full { grid-column: 1 / -1; }
      .scroll { overflow-x: auto; }
      table { width: 100%; border-collapse: collapse; font-size: 13px; }
      th { font-size: 11px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--tx3); text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); white-space: nowrap; }
      td { padding: 4px 8px; border-bottom: 1px solid var(--line); vertical-align: middle; }
      td input[type="text"] { height: 30px; }
      .bulk { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; padding: 8px 12px; border-radius: var(--r2); background: var(--accs); }
      .haact { display: grid; gap: 6px; padding: 8px 0; border-bottom: 1px solid var(--line); }
      details.pick summary { cursor: pointer; font-size: 12px; font-weight: 600; color: var(--acc); min-height: 28px; display: flex; align-items: center; }
      .why { color: var(--tx3); font-size: 12.5px; margin: 0; }
      .errdot { width: 10px; height: 10px; border-radius: 50%; background: var(--bad); box-shadow: 0 0 0 2px var(--sf); }
      @media (max-width: 960px) { .page { grid-template-columns: minmax(0, 1fr); } .dpad { grid-template-columns: repeat(3, minmax(90px, 1fr)); } }
      @media (max-width: 720px) { .gallery { grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; } .dcard .top { height: 84px; } .dcard .top .art { width: 70px; height: 52px; } .keys { grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); } .key { height: 52px; } .add { min-height: 150px; } }
    `,
  ];
  @property() selected = '';
  @state() private view: 'keypad' | 'list' = 'keypad';
  @state() private edit: CodeEdit | null = null;
  @state() private sel: string[] = [];
  @state() private adding: { where: 'inputs' | 'commands'; name: string } | null = null;
  @state() private bulkPorts = 7;
  private ha = new StudioState(this.store);

  override connectedCallback(): void {
    super.connectedCallback();
    this.ha.addEventListener('change', this.onHa);
    void this.ha.loadCapabilities();
  }
  override disconnectedCallback(): void {
    this.ha.removeEventListener('change', this.onHa);
    super.disconnectedCallback();
  }
  private onHa = () => this.requestUpdate();

  private get devices(): Device[] {
    return this.store.config.devices ?? [];
  }
  private put(cfg: ReturnType<typeof setCommand>, coalesce?: string, msg?: string) {
    this.store.replace(cfg, coalesce);
    if (msg) this.store.toast(msg, 'info', true);
  }
  private openProblem(d: Device, p: Problem) {
    const o = p.open;
    if (!o) return;
    const code = o.where === 'power' ? d.power?.[o.name as 'on' | 'off' | 'toggle'] : d[o.where]?.[o.name];
    if (code) this.edit = { where: o.where, name: o.name, code: JSON.parse(JSON.stringify(code)) as IrAction };
  }
  private errorsOf(index: number): number {
    return this.store.errors.filter((e) => e.path[0] === 'devices' && e.path[1] === index).length;
  }

  // ------------------------------------------------------------ guards
  private async confirmDelete(what: string, refs: Ref[]): Promise<boolean> {
    if (!refs.length) return true;
    const list = refs.slice(0, 8).map(describeRef).join('; ');
    return confirmAction(this, {
      title: `Delete ${what}?`,
      body: `It is used by: ${list}${refs.length > 8 ? `; and ${refs.length - 8} more` : ''}. Deleting it clears those places (keys do nothing, buttons lose their action, steps are removed). You can undo this.`,
      ok: 'Delete Anyway',
      danger: true,
    });
  }
  private async removeItem(d: Device, where: 'commands' | 'inputs', name: string) {
    const kind = where === 'commands' ? 'command' : 'input';
    const refs = findRefs(this.store.config, kind === 'command' ? { device: d.id, command: name } : { device: d.id, input: name });
    if (!(await this.confirmDelete(`${kind} "${name}"`, refs))) return;
    this.put(deleteItem(this.store.config, d.id, kind, name), undefined, `Deleted ${kind} ${name}`);
    this.edit = null;
  }
  private async removeDev(d: Device) {
    const refs = findRefs(this.store.config, { device: d.id });
    const ok = await confirmAction(this, {
      title: `Delete ${d.name ?? d.id}?`,
      body: refs.length ? `It is used by ${refs.length} place${refs.length === 1 ? '' : 's'}: ${refs.slice(0, 6).map(describeRef).join('; ')}${refs.length > 6 ? '; …' : ''}. They are cleared too. You can undo this.` : 'Its commands are removed from the draft. You can undo this.',
      ok: 'Delete Device',
      danger: true,
    });
    if (!ok) return;
    this.put(deleteDevice(this.store.config, d.id), undefined, `Deleted device ${d.name ?? d.id}`);
    location.hash = hashFor('devices');
  }

  // ------------------------------------------------------------ code editor events
  private saveCode(d: Device, s: CodeSave) {
    let cfg = this.store.config;
    if (s.where === 'power') cfg = setPowerCode(cfg, d.id, s.name as 'on' | 'off' | 'toggle', s.code);
    else {
      const kind = s.where === 'commands' ? 'command' : 'input';
      const existing = s.name ? (s.where === 'commands' ? d.commands : d.inputs)?.[s.name] : undefined;
      const full = existing ? null : capProblem(cfg, kind, 1, isHaDevice(d));
      if (full) {
        this.store.toast(full, 'error');
        return;
      }
      if (s.name && s.name !== s.newName) cfg = renameItem(cfg, d.id, kind, s.name, s.newName);
      cfg = (s.where === 'commands' ? setCommand : setInput)(cfg, d.id, s.newName, s.code);
    }
    this.put(cfg, undefined, s.name ? `Saved ${s.newName || s.name}` : `Added ${s.newName}`);
    this.edit = null;
  }
  private dup(d: Device, name: string) {
    const full = capProblem(this.store.config, 'command', 1, isHaDevice(d));
    if (full) {
      this.store.toast(full, 'error');
      return;
    }
    const r = duplicateCommand(this.store.config, d.id, name);
    this.put(r.config, undefined, `Duplicated as ${r.name}`);
    this.edit = { where: 'commands', name: r.name, code: JSON.parse(JSON.stringify(r.config.devices?.find((x) => x.id === d.id)?.commands?.[r.name] ?? {})) as IrAction };
  }

  // ------------------------------------------------------------ gallery
  private card(d: Device, i: number) {
    const ps = powerStatus(d);
    const errs = this.errorsOf(i);
    const ha = isHaDevice(d);
    const inputs = Object.keys(d.inputs ?? {}).length;
    return html`<a class="dcard" href=${hashFor('devices', d.id)} aria-label="${d.name ?? d.id}: ${commandCount(d)} commands, ${ps.text}">
      <div class="top">${deviceArt(d)}${errs ? html`<span class="errdot" title="${errs} error${errs === 1 ? '' : 's'}"></span>` : nothing}</div>
      <div class="body">
        <h2>${dn(d.name, d.id)}</h2>
        <span class="src">${sourceLabel(d)}</span>
        <div class="chips">
          <span class="chip ${ps.tone === 'ok' ? 'ok' : ps.tone === 'ir' ? 'ir' : ''}">${ps.text}</span>
          <span class="chip">${commandCount(d)} command${commandCount(d) === 1 ? '' : 's'}</span>
          ${inputs ? html`<span class="chip">${inputs} input${inputs === 1 ? '' : 's'}</span>` : nothing}
          ${ha ? html`<span class="chip acc">Home Assistant</span>` : nothing}          ${errs ? html`<span class="chip bad">${errs} error${errs === 1 ? '' : 's'}</span>` : nothing}
        </div>
      </div>
    </a>`;
  }
  private gallery() {
    const all = this.devices;
    return html`
      <div class="head"><h1>Devices</h1><span class="chip">${all.length} / ${LIMITS.devices}</span><span class="muted">What the hub can control. Open one to see its keypad.</span></div>
      <div class="gallery" role="list" aria-label="Devices">
        ${all.map((d, i) => html`<div role="listitem" class="cont">${this.card(d, i)}</div>`)}
        <div role="listitem" class="addw">${all.length >= LIMITS.devices ? html`<span class="add" aria-disabled="true" title=${capProblem(this.store.config, 'device') ?? ''}><span class="plus">${icon('plus')}</span>Add Device<span class="muted">The hub holds at most ${LIMITS.devices} devices.</span></span>` : html`<a class="add" href=${hashFor('devices', 'new')}><span class="plus">${icon('plus')}</span>Add Device<span class="muted">From the IR archive, pasted codes or Home Assistant</span></a>`}</div>
      </div>
      ${all.length === 0 ? html`<p class="muted">No devices yet. Add your first one to get started.</p>` : nothing}`;
  }

  // ------------------------------------------------------------ keypad
  private key(d: Device, where: 'commands' | 'inputs', name: string, code: IrAction) {
    const bad = this.store.errors.some((e) => e.path[0] === 'devices' && e.path[2] === where && e.path[3] === name && this.devices[Number(e.path[1])]?.id === d.id);
    const g = where === 'inputs' ? ('inputs' as Group) : groupCommand(name);
    return html`<div class="key ${bad ? 'bad' : ''}">
      <button class="k" aria-label="Edit ${name}" @click=${() => (this.edit = { where, name, code: JSON.parse(JSON.stringify(code)) as IrAction })}>
        <b>${stockIconSvg(commandIconId(name, g))}<span>${displayCommand(name)}</span></b><small>${summarize(code)}${code.port_mask ? ` · ports ${code.port_mask}` : ''}${code.repeats && code.repeats > 1 ? ` · ×${code.repeats}` : ''}</small>
      </button>
      <hh-send-ir variant="round" ?ha=${d.kind === 'ha'} .code=${code} name=${name}></hh-send-ir>
    </div>`;
  }
  private placeholder(d: Device, where: 'commands' | 'inputs', label: string) {
    const full = capProblem(this.store.config, where === 'commands' ? 'command' : 'input', 1, isHaDevice(d));
    if (full) return html`<div class="key ph"><button class="k" disabled aria-disabled="true" title=${full}>+ ${label}</button><span class="why" role="note">${full}</span></div>`;
    return html`<div class="key ph"><button class="k" @click=${() => (this.edit = { where, name: null, code: { proto: 'nec', addr: 0, cmd: 0 } })}>+ ${label}</button></div>`;
  }
  private powerKeys(d: Device) {
    const p = d.power ?? {};
    return (['on', 'off', 'toggle'] as const).filter((k) => p[k]).map(
      (k) => html`<div class="key"><button class="k" aria-label="Edit power ${k}" @click=${() => (this.edit = { where: 'power', name: k, code: JSON.parse(JSON.stringify(p[k])) as IrAction })}>
        <b>${stockIconSvg(commandIconId('power'))}<span>Power ${k === 'on' ? 'On' : k === 'off' ? 'Off' : 'Toggle'}</span></b><small>${summarize(p[k])}</small></button>
        <hh-send-ir variant="round" ?ha=${d.kind === 'ha'} .code=${p[k]} name="power ${k}"></hh-send-ir></div>`,
    );
  }
  private groupBlock(d: Device, g: Group, items: [string, IrAction][], where: 'commands' | 'inputs') {
    const nav = g === 'navigation';
    const dpadNames: Record<string, string> = {};
    const rest: [string, IrAction][] = [];
    if (nav) {
      const slot = (n: string) => ({ up: 'up', down: 'down', left: 'left', right: 'right', ok: 'ok', select: 'ok', enter: 'ok' })[n.toLowerCase()];
      for (const it of items) {
        const s = slot(it[0]);
        if (s && !dpadNames[s]) dpadNames[s] = it[0];
        else rest.push(it);
      }
    }
    const numbered = orderBy(items, (e) => e[0]);
    const dp = ['', 'up', '', 'left', 'ok', 'right', '', 'down', ''];
    const map = new Map(items);
    return html`<section class="grp" aria-label="${pretty(g)} keys">
      <h2>${stockIconSvg(groupIconId(g))} ${pretty(g)}<span class="muted">${items.length}</span></h2>
      ${nav && Object.keys(dpadNames).length ? html`<div class="dpad">${dp.map((s) => (s && dpadNames[s] ? this.key(d, where, dpadNames[s] as string, map.get(dpadNames[s] as string) as IrAction) : html`<span></span>`))}</div>` : nothing}
      <div class=${g === 'numbers' && numbered.length >= 8 ? 'numpad' : 'keys'}>${(nav ? orderUnits(rest, (e) => e[0]) : g === 'numbers' ? numbered.map((e): [typeof e] => [e]) : orderUnits(numbered, (e) => e[0])).map((u) => (u.length === 2 ? html`<div class="pair">${this.key(d, where, u[0][0], u[0][1])}${this.key(d, where, u[1][0], u[1][1])}</div>` : this.key(d, where, u[0][0], u[0][1])))}${g === 'inputs' ? this.placeholder(d, 'inputs', 'input') : nothing}</div>
    </section>`;
  }
  private keypad(d: Device) {
    const cmds = Object.entries(d.commands ?? {});
    const by = new Map<Group, [string, IrAction][]>(GROUPS.map((g) => [g, []]));
    for (const c of cmds) by.get(groupCommand(c[0]) as Group)?.push(c);
    return html`
      <section class="grp" aria-label="Power keys"><h2>${stockIconSvg(groupIconId('power'))} Power</h2><div class="keys">${this.powerKeys(d)}${(by.get('power') ?? []).map(([n, c]) => this.key(d, 'commands', n, c))}${!d.power ? html`<span class="muted">No power control (see Power on the right)</span>` : nothing}</div></section>
      ${GROUPS.filter((g) => g !== 'power' && g !== 'inputs' && (by.get(g)?.length ?? 0) > 0).map((g) => this.groupBlock(d, g, by.get(g) ?? [], 'commands'))}
      ${this.groupBlock(d, 'inputs', Object.entries(d.inputs ?? {}), 'inputs')}
      <div class="keys">${this.placeholder(d, 'commands', 'command')}</div>`;
  }

  // ------------------------------------------------------------ list view
  private list(d: Device) {
    const rows = Object.entries(d.commands ?? {});
    const ha = this.ha.haWritable;
    const all = this.sel.length === rows.length && rows.length > 0;
    const mask = this.bulkPorts;
    return html`
      ${this.sel.length
        ? html`<div class="bulk" role="region" aria-label="Bulk edit"><b>${this.sel.length} selected</b>
            <div class="ports" role="group" aria-label="IR ports">${[1, 2, 4].map((bit, i) => html`<button aria-pressed=${(mask & bit) !== 0} aria-label="Port ${i + 1}" @click=${() => (this.bulkPorts = (mask ^ bit) || 7)}>${i + 1}</button>`)}</div>
            <button class="btn sm" @click=${() => this.put(mapCodes(this.store.config, d.id, this.sel, (c) => withPorts(c, mask)), undefined, `Set IR ports on ${this.sel.length} commands`)}>Set IR Ports</button>
            ${ha ? html`<button class="btn sm" @click=${() => this.put(mapCodes(this.store.config, d.id, this.sel, (c) => withRepeats(c, 2)), undefined, 'Set repeats to 2')}>Repeat ×2</button>` : nothing}
            <button class="btn sm danger" @click=${() => void this.bulkDelete(d)}>Delete</button></div>`
        : nothing}
      <div class="scroll"><table aria-label="Commands of ${d.name ?? d.id}">
        <thead><tr><th><input type="checkbox" aria-label="Select all" .checked=${all} @change=${(e: Event) => (this.sel = (e.target as HTMLInputElement).checked ? rows.map((r) => r[0]) : [])} /></th><th>Name</th><th>Code</th><th>Ports</th>${ha ? html`<th>Repeats</th>` : nothing}<th></th></tr></thead>
        <tbody>${rows.map(([n, c]) => {
          const prob = this.rowName?.[0] === n ? nameProblem(d, this.rowName[1], n) : null;
          return html`<tr>
            <td><input type="checkbox" aria-label="Select ${n}" .checked=${this.sel.includes(n)} @change=${(e: Event) => (this.sel = (e.target as HTMLInputElement).checked ? [...this.sel, n] : this.sel.filter((x) => x !== n))} /></td>
            <td><input type="text" aria-label="Name of ${n}" .value=${n} @input=${(e: Event) => (this.rowName = [n, (e.target as HTMLInputElement).value])} @change=${(e: Event) => this.renameRow(d, n, (e.target as HTMLInputElement).value)} />${prob ? html`<span class="hint" role="alert">${prob}</span>` : nothing}<span class="hint">${byteCounter(this.rowName?.[0] === n ? this.rowName[1] : n).used}/31 B</span></td>
            <td class="mono">${summarize(c)}</td><td class="mono">${c.port_mask ?? 'all'}</td>${ha ? html`<td class="mono">${c.repeats ?? 1}</td>` : nothing}
            <td><button class="btn sm" @click=${() => (this.edit = { where: 'commands', name: n, code: JSON.parse(JSON.stringify(c)) as IrAction })}>Edit</button> <hh-send-ir ?ha=${d.kind === 'ha'} .code=${c} name=${n}></hh-send-ir></td></tr>`;
        })}</tbody></table></div>`;
  }
  private rowName: [string, string] | null = null;
  private renameRow(d: Device, from: string, to: string) {
    const v = to.trim();
    if (v === from || nameProblem(d, v, from)) {
      this.rowName = null;
      this.requestUpdate();
      return;
    }
    this.rowName = null;
    this.put(renameItem(this.store.config, d.id, 'command', from, v), undefined, `Renamed ${from} to ${v}`);
  }
  private async bulkDelete(d: Device) {
    const refs = this.sel.flatMap((n) => findRefs(this.store.config, { device: d.id, command: n }));
    if (!(await this.confirmDelete(`${this.sel.length} commands`, refs))) return;
    let cfg = this.store.config;
    for (const n of this.sel) cfg = deleteItem(cfg, d.id, 'command', n);
    this.put(cfg, undefined, `Deleted ${this.sel.length} commands`);
    this.sel = [];
  }

  // ------------------------------------------------------------ side panels
  private powerPanel(d: Device) {
    const mode = powerMode(d);
    const modes: [PowerMode, string][] = [['both', 'On and Off'], ['toggle', 'One Toggle'], ['none', 'None']];
    return html`<section class="card" aria-label="Power control">
      <h2>Power</h2>
      <div class="seg" role="group" aria-label="Power control">${modes.map(([m, label]) => html`<button aria-pressed=${mode === m} @click=${() => this.put(setPowerMode(this.store.config, d.id, m))}>${label}</button>`)}</div>
      <p class="why mt">${mode === 'both' ? 'Separate on and off codes: the hub always knows what it sends.' : mode === 'toggle' ? 'One code switches it on and off. It can get out of step if you use the original remote.' : 'The hub will not switch this device on or off.'}</p>
      ${(['on', 'off', 'toggle'] as const).filter((k) => d.power?.[k]).map((k) => html`<div class="row"><span class="l"><b>${k === 'toggle' ? 'Toggle' : `Power ${k === 'on' ? 'On' : 'Off'}`}</b><small>${summarize(d.power?.[k])}</small></span><span><button class="btn sm" @click=${() => (this.edit = { where: 'power', name: k, code: JSON.parse(JSON.stringify(d.power?.[k])) as IrAction })}>Edit</button> <hh-send-ir variant="round" ?ha=${d.kind === 'ha'} .code=${d.power?.[k]} name="power ${k}"></hh-send-ir></span></div>`)}
    </section>`;
  }
  private settingsPanel(d: Device) {
    const mask = Object.values(d.commands ?? {}).every((c) => (c.port_mask ?? 7) === (Object.values(d.commands ?? {})[0]?.port_mask ?? 7)) ? (Object.values(d.commands ?? {})[0]?.port_mask ?? 7) : 0;
    const refs = findRefs(this.store.config, { device: d.id });
    return html`<section class="card" aria-label="Settings">
      <h2>Settings</h2>
      <label class="f">Settle Time (ms)<input type="number" min="0" max="60000" step="100" aria-label="Settle time in milliseconds" .value=${String(d.settle_ms ?? 0)} @change=${(e: Event) => this.put(patchDevice(this.store.config, d.id, { settle_ms: Math.max(0, Math.min(60000, Number((e.target as HTMLInputElement).value) || 0)) || undefined }), `settle${d.id}`)} />
        <span class="hint">How long the hub waits after power-on or an input change before the next step. TVs need 2000 or more.</span></label>
      <div class="f mt">IR ports for all commands
        <div class="ports" role="group" aria-label="IR ports for all commands">${[1, 2, 4].map((bit, i) => html`<button aria-pressed=${mask !== 0 && (mask & bit) !== 0} aria-label="Port ${i + 1}" @click=${() => { const m = ((mask || 7) ^ bit) || 7; this.put(mapCodes(this.store.config, d.id, null, (c) => withPorts(c, m)), undefined, `IR ports set for every command`); }}>${i + 1}</button>`)}</div>
        <span class="hint">${mask === 0 ? 'Commands use different ports. Edit one to see its ports.' : mask === 7 ? 'All three ports.' : 'Only the lit ports send.'} Pick ports to aim the IR blasters.</span></div>
      ${refs.length
        ? html`<details class="used mt" open><summary>Used by ${refs.length} place${refs.length === 1 ? '' : 's'}</summary>
            <ul>${refs.slice(0, 30).map((r) => html`<li><a href=${refHref(r)}>${describeRef(r)}</a></li>`)}${refs.length > 30 ? html`<li class="muted">and ${refs.length - 30} more</li>` : nothing}</ul></details>`
        : html`<p class="why mt">Not used by any activity or key yet.</p>`}
    </section>`;
  }

  // ------------------------------------------------------------ Home Assistant device
  private haAction(label: string, current: IrAction | undefined, onPick: (a: IrAction | null) => void, extra?: TemplateResult) {
    const text = current?.ha ? `${current.ha.service} ${Array.isArray(current.ha.target.entity_id) ? current.ha.target.entity_id.join(', ') : current.ha.target.entity_id}${current.ha.data ? ` ${JSON.stringify(current.ha.data)}` : ''}` : 'Not set';
    return html`<div class="haact"><div class="row bare"><span class="l"><b>${label}</b><small>${text}</small></span>${extra ?? nothing}</div>
      <details class="pick"><summary>${current ? 'Change Action' : 'Choose Action'}</summary>
        <hh-action-picker .host=${HOST(this.ha)} .value=${current ?? null} mode="widget" @pick=${(e: CustomEvent<Source>) => { const a = sourceAction(e.detail); if (a && 'ha' in a) onPick(a as unknown as IrAction); }}></hh-action-picker></details></div>`;
  }
  private haDevice(d: Device) {
    const ok = this.ha.haWritable;
    const st = this.ha;
    void st.loadHa();
    const set = (cfg: ReturnType<typeof setCommand>) => this.put(cfg);
    const add = this.adding;
    return html`
      ${ok ? nothing : html`<div class="banner ir" role="note">Home Assistant devices cannot be edited here: this add-on version does not store Home Assistant actions yet (the add-on reports ha_actions: off).</div>`}
      <section class="card" aria-label="Entity"><h2>Entity</h2>
        <label class="f">Where Its State Comes From<input type="text" list="hh-dev-ents" aria-label="Entity" .value=${d.entity ?? ''} placeholder="media_player.shield" ?disabled=${!ok} @change=${(e: Event) => set(patchDevice(this.store.config, d.id, { entity: (e.target as HTMLInputElement).value.trim() || undefined }))} />
          <datalist id="hh-dev-ents">${(st.entities ?? []).slice(0, 300).map((e) => html`<option value=${e.entity_id}>${e.name ?? ''}</option>`)}</datalist>
          <span class="hint">Power shows as on unless this entity is off, standby, unavailable or unknown.</span></label></section>
      <section class="card" aria-label="Power control"><h2>Power</h2>
        ${(['on', 'off'] as const).map((k) => this.haAction(`Power ${k === 'on' ? 'On' : 'Off'}`, d.power?.[k], (a) => set(setPowerCode(this.store.config, d.id, k, a))))}</section>
      ${(['inputs', 'commands'] as const).map((where) => html`<section class="card" aria-label=${pretty(where)}><h2>${pretty(where)}</h2>
        ${Object.entries(d[where] ?? {}).map(([n, a]) => this.haAction(n, a, (x) => { if (x) set((where === 'inputs' ? setInput : setCommand)(this.store.config, d.id, n, x)); }, html`<button class="btn sm danger" @click=${() => void this.removeItem(d, where, n)}>Delete</button>`))}
        ${add?.where === where
          ? html`<div class="haact"><label class="f">Name<input type="text" aria-label="New name" .value=${add.name} @input=${(e: Event) => (this.adding = { where, name: (e.target as HTMLInputElement).value })} /></label>
              ${nameProblem(d, add.name, undefined, where === 'inputs' ? 'input' : 'command') ? html`<span class="hint" role="alert">${nameProblem(d, add.name, undefined, where === 'inputs' ? 'input' : 'command')}</span>` : html`<hh-action-picker .host=${HOST(st)} mode="widget" @pick=${(e: CustomEvent<Source>) => { const a = sourceAction(e.detail); if (a && 'ha' in a) { set((where === 'inputs' ? setInput : setCommand)(this.store.config, d.id, add.name.trim(), a as unknown as IrAction)); this.adding = null; } }}></hh-action-picker>`}
              <button class="btn sm" @click=${() => (this.adding = null)}>Cancel</button></div>`
          : html`<button class="btn sm" ?disabled=${!ok} @click=${() => (this.adding = { where, name: '' })}>+ ${where === 'inputs' ? 'Input' : 'Command'}</button>`}</section>`)}`;
  }

  // ------------------------------------------------------------ page
  private devicePage(d: Device) {
    const ha = isHaDevice(d);
    const bt = d.kind === 'bt';
    if (bt) void import('./hh-bt-device');
    const idx = this.devices.indexOf(d);
    const bytes = byteCounter(d.name ?? d.id);
    return html`
      <div class="dbar">
        <a class="back" href=${hashFor('devices')}>← All Devices</a><span class="sp"></span>
        ${ha || bt ? nothing : html`<div class="seg" role="group" aria-label="View"><button aria-pressed=${this.view === 'keypad'} @click=${() => (this.view = 'keypad')}>Keypad</button><button aria-pressed=${this.view === 'list'} @click=${() => (this.view = 'list')}>List View</button></div>`}
        <button class="btn danger" @click=${() => void this.removeDev(d)}>Delete Device</button>
      </div>
      ${problemList(problemsFor(this.store.errors, 'devices', idx), (p) => p.open && this.openProblem(d, p))}
      <div class="dbar">
        <span class="chip acc">${KIND_LABEL[deviceKind(d)]}</span>
        <details class="iconpick"><summary aria-label="Device icon">${deviceArt(d, 'dic')}<span>Icon</span></summary>
          <hh-icon-picker .value=${d.icon ?? ''} @hh-icon=${(e: CustomEvent<string | undefined>) => this.put(patchDevice(this.store.config, d.id, { icon: e.detail }), undefined, e.detail ? `Icon of ${d.name ?? d.id} set to ${e.detail}` : `${d.name ?? d.id} uses the default icon`)}></hh-icon-picker></details>
        <input class="name" aria-label="Device name" .value=${d.name ?? d.id} @change=${(e: Event) => this.put(patchDevice(this.store.config, d.id, { name: trimBytes((e.target as HTMLInputElement).value.trim()) || undefined }), `name${d.id}`)} />
        <span class="muted">${bytes.used}/31 B · id <span class="mono">${d.id}</span> · ${sourceLabel(d)}${this.errorsOf(idx) ? ` · ${this.errorsOf(idx)} errors` : ''}</span>
      </div>
      ${bt
        ? html`<div class="page"><div class="side full"><hh-bt-device .device=${d}></hh-bt-device></div></div>`
        : ha
        ? html`<div class="page"><div class="side full">${this.haDevice(d)}</div></div>`
        : html`<div class="page">
            <div>${this.view === 'keypad' ? this.keypad(d) : this.list(d)}</div>
            <div class="side">${this.powerPanel(d)}${this.settingsPanel(d)}</div>
          </div>`}
      <hh-code-dialog .device=${this.edit ? d : null} .edit=${this.edit} ?ha25=${this.ha.haWritable}
        @hh-code-save=${(e: CustomEvent<CodeSave>) => this.saveCode(d, e.detail)} @hh-code-close=${() => (this.edit = null)}
        @hh-code-dup=${(e: CustomEvent<string>) => this.dup(d, e.detail)}
        @hh-code-delete=${(e: CustomEvent<CodeEdit>) => e.detail.name && e.detail.where !== 'power' && void this.removeItem(d, e.detail.where, e.detail.name)}></hh-code-dialog>`;
  }

  protected override render() {
    const sel = this.selected;
    const d = this.devices.find((x) => x.id === sel);
    if (sel && !d) return html`<div class="card empty"><h2>That Device Is Gone</h2><a class="btn" href=${hashFor('devices')}>All Devices</a></div>`;
    return d ? this.devicePage(d) : this.gallery();
  }
}
