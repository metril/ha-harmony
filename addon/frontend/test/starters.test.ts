import { describe, expect, it } from 'vitest';
import type { Config } from '../src/lib/model';
import type { Device } from '../src/lib/model';
import vectors from '../../../tests/vectors/inputs.json';
import { derivedInputsNote, deviceInputs, inferDeviceIcon, inputIcon, isHardKey, labelMeter, normalizeInput, pageOverflow, templatePages } from '../src/lib/studio-model';
import { bestIcon, suggestIcons } from '../src/lib/icon-suggest';

const ir = { proto: 'nec', addr: 1, cmd: 1 };
const cmds = (...n: string[]) => Object.fromEntries(n.map((k) => [k, ir]));
const cfg = (): Config =>
  ({
    devices: [
      { id: 'shield', name: 'Shield TV', commands: cmds('Plex', 'YouTube', 'Home', 'Settings', 'DirectionUp', 'DirectionDown', 'vol_up', 'mute', 'play_pause', 'replay_10', 'skip_10', 'Subtitles', 'Num1', 'Num2', 'Num3', 'Num4', 'Num5', 'Num6', 'Num7', 'Num8', 'Num9', 'Num0', 'enter') },
      { id: 'tv', name: 'LG TV', commands: cmds('power_on', 'power_off', 'InputHdmi1', 'InputHdmi2', 'picture_mode', 'aspect_ratio', 'menu', 'vol_up') },
    ],
    activities: [{ id: 'watch', name: 'Watch Shield', devices: [{ device: 'shield' }] }],
    pages: [],
  }) as unknown as Config;

describe('starter pages', () => {
  it('hard keys are recognised', () => {
    for (const k of ['vol_up', 'DirectionUp', 'mute', 'play_pause', 'FastForward', 'menu', 'Return']) expect(isHardKey(k), k).toBe(true);
    for (const k of ['Plex', 'replay_10', 'skip_forward_10', 'Subtitles', 'enter', 'InputHdmi1']) expect(isHardKey(k), k).toBe(false);
  });

  it('the activity starter makes Home, Apps, Extras and Numbers without hard keys or device prefixes', () => {
    const pages = templatePages(cfg(), 'activity', { activity: 'watch' });
    expect(pages.map((p) => p.title)).toEqual(['Shield TV', 'Extras', 'Numbers']);
    const labels = pages.flatMap((p) => p.widgets.map((w) => w.label));
    expect(labels).toContain('Plex');
    expect(labels.some((l) => /shield/i.test(l ?? ''))).toBe(false);
    const commands = pages.flatMap((p) => p.widgets.map((w) => (w['action'] as { command?: string } | undefined)?.command));
    expect(commands.some((c) => c && isHardKey(c))).toBe(false);
    expect(pages[0]?.activity).toBe('watch');
    // Home links to the other pages
    expect(pages[0]?.widgets.filter((w) => w.nav).map((w) => w.nav)).toEqual(['extras', 'numbers']);
  });

  it('Numbers is a 3x4 pill keypad of single digits', () => {
    const num = templatePages(cfg(), 'numbers')[0]!;
    expect([num.cols, num.rows]).toEqual([3, 4]);
    expect(num.widgets.filter((w) => w.shape === 'pill')).toHaveLength(11);
    expect(num.widgets.slice(0, 9).map((w) => w.label)).toEqual(['1', '2', '3', '4', '5', '6', '7', '8', '9']);
  });

  it('the device list takes the Devices hard key (favorites) unless the config has one', () => {
    expect(templatePages(cfg(), 'devices')[0]?.kind).toBe('favorites');
    const c = { ...cfg(), pages: [{ id: 'f', kind: 'favorites', cols: 1, rows: 1, widgets: [] }] } as ReturnType<typeof cfg>;
    expect(templatePages(c, 'devices')[0]?.kind).toBeUndefined();
  });

  it('the device starter is a fixed page: a glyph row per device, keys, inline inputs and All Commands', () => {
    const pages = templatePages(cfg(), 'devices');
    expect(pages.map((p) => p.id)).toEqual(['devices', 'dev_shield', 'dev_tv', 'dev_shield_all']);
    expect(pages.every((p) => p.layout === 'fixed' && p.cols === 12)).toBe(true);
    const list = pages[0]!;
    expect(list.widgets.map((w) => [w.label, w.nav, w.shape, w.w])).toEqual([['Shield TV', 'dev_shield', 'row', 12], ['LG TV', 'dev_tv', 'row', 12]]);
    expect(list.widgets.every((w) => !!w.icon)).toBe(true);
    const tv = pages.find((p) => p.id === 'dev_tv')!;
    const keys = tv.widgets.filter((w) => w.w === 4);
    expect(keys.map((w) => w.label)).toEqual(['Pic Mode', 'Aspect']);
    const withPower = templatePages({ devices: [{ id: 'p', name: 'P', power: { on: ir, off: ir }, commands: cmds('Tools') }], activities: [], pages: [] } as unknown as Config, 'devices')[1]!;
    expect(withPower.widgets.map((w) => [w.label, w.x, w.y, w.w])).toEqual([['Power On', 0, 0, 4], ['Power Off', 4, 0, 4], ['Tools', 8, 0, 4]]);
    expect(tv.widgets.filter((w) => w.shape === 'row').map((w) => [w.label, w.icon])).toEqual([['HDMI 1', 'hdmi1'], ['HDMI 2', 'hdmi2']]);
    expect(tv.widgets.map((w) => w.label)).not.toContain('Menu'); // a hard key
    expect(tv.widgets.some((w) => /^lg|input hdmi/i.test(w.label ?? ''))).toBe(false);
    const sh = pages.find((p) => p.id === 'dev_shield')!;
    expect(sh.widgets.at(-1)).toMatchObject({ label: 'All Commands', nav: 'dev_shield_all' });
    expect(sh.widgets.filter((w) => w.w === 4).length % 3).toBe(0);
  });

  it('5 or more inputs sit behind an Inputs row; the page height never passes the stack limit', () => {
    const c = { devices: [{ id: 'sw', name: 'Switch', inputs: Object.fromEntries(['HDMI 1', 'HDMI 2', 'HDMI 3', 'HDMI 4', 'AV'].map((k) => [k, ir])) }], activities: [], pages: [] } as unknown as Config;
    const pages = templatePages(c, 'devices');
    expect(pages.map((p) => p.id)).toEqual(['devices', 'dev_sw', 'dev_sw_in']);
    expect(pages[1]!.widgets.map((w) => [w.label, w.nav])).toEqual([['Inputs', 'dev_sw_in']]);
    expect(pages[2]!.widgets.map((w) => w.icon)).toEqual(['hdmi1', 'hdmi2', 'hdmi3', 'hdmi4', 'av']);
  });

  it('a device without anything to press gets no row', () => {
    const c = { devices: [{ id: 'x', name: 'Empty' }], activities: [], pages: [] } as unknown as Config;
    expect(templatePages(c, 'devices')[0]!.widgets).toEqual([]);
  });

  it('the favorites starter is a page of 6-wide compact cards', () => {
    const f = templatePages(cfg(), 'favorites')[0]!;
    expect(f.layout).toBe('fixed');
    expect(f.widgets[0]).toMatchObject({ label: 'Watch Shield', w: 6, compact: true, action: { activity: 'watch' } });
    expect(f.widgets.every((w) => w.w === 6 && w.compact === true && (w.x === 0 || w.x === 6))).toBe(true);
  });

  it('normalizeInput follows the shared vectors; inputs derive from commands', () => {
    for (const [name, want] of vectors as [string, string | null][]) expect(normalizeInput(name), name).toBe(want);
    expect(deviceInputs({ id: 'tv', commands: cmds('InputHdmi1', 'HDMI2', 'InputTv', 'Mute') } as unknown as Device).map((i) => i.label)).toEqual(['HDMI 1', 'HDMI 2', 'TV']);
    expect(derivedInputsNote({ id: 'tv', commands: cmds('InputHdmi1', 'InputAv', 'InputUsb', 'InputTv') } as unknown as Device)).toBe('4 inputs found in commands');
    expect(derivedInputsNote({ id: 'tv', inputs: { A: ir }, commands: cmds('InputHdmi1') } as unknown as Device)).toBe('');
  });

  it('device icons: config, brand, kind or domain, then keyword; input glyphs', () => {
    const d = (o: object) => ({ id: 'x', ...o }) as unknown as Device;
    expect(inferDeviceIcon(d({ icon: 'tv', name: 'Whatever' }))).toBe('tv');
    expect(inferDeviceIcon(d({ id: 'shield', name: 'Shield', kind: 'bt' }))).toBe('nvidia');
    expect(inferDeviceIcon(d({ id: 'shield_ha', name: 'Shield Apps', kind: 'ha', entity: 'media_player.nvidia_shield_tv' }))).toBe('grid_apps');
    expect(inferDeviceIcon(d({ name: 'Speakers' }))).toBe('speaker');
    expect(inferDeviceIcon(d({ id: 'sp', name: 'Speaker Power', kind: 'ha', entity: 'switch.speakers' }))).toBe('speaker');
    expect(inferDeviceIcon(d({ id: 'p', name: 'Lamp Power', kind: 'ha', entity: 'switch.lamp' }))).toBe('plug');
    expect(inferDeviceIcon(d({ name: 'KVM' }))).toBe('kvm');
    expect(['HDMI 3', 'AV', 'USB', 'TV', 'MacBook Pro', 'Computer 4', 'Desktop', 'Work Laptop'].map((l) => inputIcon(l))).toEqual(['hdmi3', 'av', 'usb', 'tv', 'laptop', 'pc', 'pc', 'laptop']);
  });

  it('no starter label overflows its cell (fixed-layout class budgets are checked by the studio)', () => {
    for (const id of ['activity', 'devices', 'numbers'] as const) {
      const c = cfg();
      const pages = templatePages(c, id, { activity: 'watch' });
      for (const p of pages) expect(pageOverflow(p, null), `${id}/${p.id}`).toEqual([]);
    }
  });

  it('a 1-wide cell in a 4-column grid shows the icon alone when one matches', () => {
    const home = templatePages(cfg(), 'activity', { activity: 'watch' })[0]!;
    const settings = home.widgets.find((w) => w.label === 'Settings')!;
    expect(settings.icon).toBe('settings');
    expect(settings.icon_only).toBe(true);
  });
});

describe('label meter and overflow', () => {
  const page = { id: 'p', cols: 4, rows: 5, title: 'Shield TV', widgets: [{ type: 'button', id: 'a', x: 2, y: 0, w: 2, h: 1, label: 'Shield TV YouTube', action: { device: 'shield', command: 'YouTube' } }, { type: 'button', id: 'b', x: 0, y: 1, label: 'Menu', action: { device: 'shield', command: 'menu' } }] };
  it('measures a label in pixels', () => {
    expect(labelMeter(page, null, 0)).toMatchObject({ font: 'm16', px: 100, used: 133, over: 33 });
  });
  it('lists the cut labels with advice', () => {
    const o = pageOverflow({ ...page, widgets: [page.widgets[0]!, { ...page.widgets[1]!, label: 'Shield TV Menu' }] }, null);
    expect(o.map((x) => [x.label, x.over > 0])).toEqual([['Shield TV YouTube', true], ['Shield TV Menu', true]]);
    expect(o[0]?.advice).toContain('drop the prefix');
    expect(o[1]?.advice).toContain('hard key');
  });
});

describe('icon suggestions', () => {
  it('an icon named like the command wins over keyword matches', () => {
    expect(bestIcon('vol_up')).toBe('vol_up');
    expect(bestIcon('DirectionUp')).toBe('up');
    expect(bestIcon('Settings')).toBe('settings');
  });
  it('says why it matched', () => {
    const s = suggestIcons('Source', 'Source');
    expect(s[0]?.icon.name).toBe('input');
    expect(s[0]?.reason).toContain('keyword');
  });
});
