import { describe, expect, it } from 'vitest';
import type { Config } from '../src/lib/model';
import type { Device } from '../src/lib/model';
import vectors from '../../../tests/vectors/inputs.json';
import { activityIcon, fitShort, fitTitle, derivedInputs, derivedInputsNote, deviceInputs, materializeInputs, studioChecks, inferDeviceIcon, inputIcon, isHardKey, labelMeter, normalizeInput, pageOverflow, templatePages } from '../src/lib/studio-model';
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

  it('Numbers is a fixed 3x4 pill keypad of single digits', () => {
    const num = templatePages(cfg(), 'numbers')[0]!;
    expect([num.layout, num.cols, num.rows]).toEqual(['fixed', 12, 4]);
    expect(num.widgets.every((w) => w.w === 4)).toBe(true);
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

  it('only the power buttons whose code exists are added; AV 1 and AV 2 stay separate', () => {
    const c = { devices: [{ id: 'p', name: 'P', power: { on: ir }, commands: cmds('InputAv1', 'InputAv2') }], activities: [], pages: [] } as unknown as Config;
    const pg = templatePages(c, 'devices')[1]!;
    expect(pg.widgets.map((w) => w.label)).toEqual(['Power On', 'AV 1', 'AV 2']);
  });

  it('a device without anything to press gets no row', () => {
    const c = { devices: [{ id: 'x', name: 'Empty' }], activities: [], pages: [] } as unknown as Config;
    expect(templatePages(c, 'devices')[0]!.widgets).toEqual([]);
  });

  describe('device groups', () => {
    const ha = (apps: string[], extra: object = {}) => ({ id: 'shield_ha', kind: 'ha', name: 'Shield Apps', icon: 'grid_apps', entity: 'media_player.nvidia_shield_tv', power: { on: ir, off: ir }, commands: cmds(...apps), ...extra });
    const grouped = (apps = ['Plex', 'YouTube'], over: object = {}): Config =>
      ({
        devices: [
          { id: 'shield', kind: 'bt', name: 'Shield', group: 'shield', commands: cmds('Plex Key', 'Settings', 'Subtitles') },
          { id: 'tv', name: 'TV', commands: cmds('Subtitles') },
          { ...ha(apps), group: 'shield' },
        ],
        activities: [],
        pages: [],
        ...over,
      }) as unknown as Config;

    it('one row and one page per group, named and iconed from the first member', () => {
      const pages = templatePages(grouped(), 'devices');
      expect(pages.map((p) => p.id)).toEqual(['devices', 'dev_shield', 'dev_tv']);
      expect(pages[0]!.widgets.map((w) => [w.label, w.icon, w.nav])).toEqual([['Shield', 'nvidia', 'dev_shield'], ['TV', 'tv', 'dev_tv']]);
      expect(pages[1]!.title).toBe('Shield');
    });

    it('the groups section overrides the row name and icon', () => {
      const c = grouped(undefined, { groups: { shield: { name: 'Living Box', icon: 'streamer' } } });
      expect(templatePages(c, 'devices')[0]!.widgets[0]).toMatchObject({ label: 'Living Box', icon: 'streamer' });
    });

    it('a group whose first member is the app list takes the next member glyph', () => {
      const c = grouped();
      c.devices = [{ ...c.devices![2]!, icon: undefined } as Device, c.devices![0]!];
      expect(templatePages(c, 'devices')[0]!.widgets[0]).toMatchObject({ label: 'Shield Apps', icon: 'nvidia' });
    });

    it('the page runs power, keys, inputs, app cards, then All Commands', () => {
      const c = grouped();
      c.devices![0]!.commands = cmds('Settings', 'Subtitles', 'Num1', 'Aspect', 'Zoom', 'Mode', 'Sleep', 'Eject', 'Info Panel', 'Angle', 'Repeat', 'Random', 'Clear', 'Display', 'Setup', 'Title', 'Search', 'Audio', 'Slow', 'Step', 'Marker', 'Return To');
      c.devices![0]!.inputs = { 'HDMI 1': ir } as never;
      const pages = templatePages(c, 'devices');
      const pg = pages.find((p) => p.id === 'dev_shield')!;
      const ws = pg.widgets;
      const kind = (w: (typeof ws)[number]): string => (w.label === 'All Commands' ? 'all' : w.shape === 'card' ? 'app' : w.shape === 'row' ? 'input' : w.icon === 'power' ? 'power' : 'key');
      const seq = ws.map(kind).filter((k, i, a) => a[i - 1] !== k);
      expect(seq).toEqual(['power', 'key', 'input', 'app', 'all']);
      const y = (k: string): number => Math.min(...ws.filter((w) => kind(w) === k).map((w) => w.y ?? 0));
      expect([y('power'), y('key'), y('input'), y('app'), y('all')].every((v, i, a) => i === 0 || v >= (a[i - 1] as number))).toBe(true);
      expect(ws.filter((w) => kind(w) === 'app').map((w) => [w.label, (w.action as { device: string }).device, w.compact])).toEqual([['Plex', 'shield_ha', undefined], ['YouTube', 'shield_ha', undefined]]);
      expect(pages.some((p) => p.id === 'dev_shield_all')).toBe(true);
      expect(pg.widgets.at(-1)).toMatchObject({ label: 'All Commands', nav: 'dev_shield_all' });
    });

    it('apps past 4 turn compact, two a row', () => {
      const pg = templatePages(grouped(['Plex', 'YouTube', 'Kodi', 'Netflix', 'Disney', 'Prime']), 'devices').find((p) => p.id === 'dev_shield')!;
      const cards = pg.widgets.filter((w) => w.shape === 'card');
      expect(cards).toHaveLength(6);
      expect(cards.every((w) => w.compact && w.w === 6)).toBe(true);
      expect(cards.map((w) => (w.y ?? 0) - (cards[0]!.y ?? 0))).toEqual([0, 0, 1, 1, 2, 2]);
    });

    it('too much content pages: 8 compact cards keep at least two key rows, the rest go to All Commands, no label is cut', () => {
      const c = grouped(['Plex', 'YouTube', 'Kodi', 'Netflix', 'Disney', 'Prime', 'Hulu', 'Max']);
      c.devices![0]!.commands = cmds('Settings', 'Subtitles', 'Num1', 'Aspect', 'Zoom', 'Mode', 'Sleep', 'Eject', 'Angle', 'Repeat', 'Random', 'Clear');
      const pages = templatePages(c, 'devices');
      const pg = pages.find((p) => p.id === 'dev_shield')!;
      expect(pg.widgets.filter((w) => w.shape === 'card')).toHaveLength(8);
      const keys = pg.widgets.filter((w) => w.w === 4 && w.shape !== 'card');
      expect(keys.length).toBeGreaterThanOrEqual(6);
      expect(pg.widgets.at(-1)).toMatchObject({ label: 'All Commands' });
      expect(pages.find((p) => p.id === 'dev_shield_all')!.widgets.length).toBeGreaterThan(0);
      expect(pageOverflow(pg, null)).toEqual([]);
    });

    it('inputs of the members merge without duplicates', () => {
      const c = grouped();
      c.devices![0]!.commands = cmds('Settings', 'InputHdmi1');
      (c.devices![2] as Device).inputs = { 'HDMI 1': ir, 'HDMI 2': ir } as never;
      const pg = templatePages(c, 'devices').find((p) => p.id === 'dev_shield')!;
      const rows = pg.widgets.filter((w) => w.shape === 'row');
      expect(rows.map((w) => w.label)).toEqual(['HDMI 1', 'HDMI 2']);
    });

    it('apps keep their HA device action so the app status binding still resolves', () => {
      const pg = templatePages(grouped(), 'devices').find((p) => p.id === 'dev_shield')!;
      const cards = pg.widgets.filter((w) => w.shape === 'card');
      expect(cards.map((w) => w.action)).toEqual([{ device: 'shield_ha', command: 'Plex' }, { device: 'shield_ha', command: 'YouTube' }]);
      expect(cards.map((w) => w.icon)).toEqual(['plex', 'youtube']);
    });
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
  const page = { id: 'p', cols: 4, rows: 5, title: 'Shield TV', widgets: [{ type: 'button', id: 'a', x: 2, y: 0, w: 2, h: 1, label: 'Shield TV YouTube Premium Music Player Edition', action: { device: 'shield', command: 'YouTube' } }, { type: 'button', id: 'b', x: 0, y: 1, label: 'Menu', action: { device: 'shield', command: 'menu' } }] };
  it('measures a label in pixels', () => {
    expect(labelMeter(page, null, 0)).toMatchObject({ font: 'b13', px: 172 }); // a 4-column cell is short: a compact card of two lines
    expect(labelMeter(page, null, 0)?.over).toBeGreaterThan(0);
  });
  it('lists the cut labels with advice', () => {
    const o = pageOverflow({ ...page, widgets: [page.widgets[0]!, { ...page.widgets[1]!, label: 'Shield TV Menu' }] }, null);
    expect(o.map((x) => [x.label, x.over > 0])).toEqual([['Shield TV YouTube Premium Music Player Edition', true], ['Shield TV Menu', true]]);
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

describe('visual pass: FIXED templates, checks and input editing', () => {
  const many = (n: number) => cmds(...Array.from({ length: n }, (_, i) => `App${i + 1}`));
  const cfgInputs = (): Config =>
    ({
      devices: [
        { id: 'tv', name: 'Living TV', commands: cmds('InputHdmi1', 'InputHdmi2', 'InputHdmi3', 'Tools', 'power_on') },
        { id: 'box', name: 'Box', commands: many(6) },
      ],
      activities: [{ id: 'watch', name: 'Shield TV', devices: [{ device: 'tv' }, { device: 'box' }] }],
      pages: [],
    }) as unknown as Config;

  it('Home is activity cards (6 wide), then rows to the pages that exist, then All Off', () => {
    const c = { ...cfgInputs(), pages: [{ id: 'devices', cols: 12, rows: 1, widgets: [] }, { id: 'favorites', cols: 12, rows: 1, widgets: [] }] } as unknown as Config;
    const [home] = templatePages(c, 'activities');
    expect([home!.layout, home!.cols, home!.kind]).toEqual(['fixed', 12, 'home']);
    expect(home!.widgets[0]).toMatchObject({ label: 'Shield TV', w: 6, x: 0, y: 0, align: 'left', icon: 'nvidia', icon_only: false });
    expect(home!.widgets.slice(1).map((w) => [w.label, w.shape, w.nav ?? null])).toEqual([['Devices', 'row', 'devices'], ['All Off', 'row', null], ['Favorites', 'row', 'favorites']]);
    expect(home!.widgets.find((w) => w.label === 'All Off')).toMatchObject({ role: 'danger', action: { activity: 'off' } });
  });

  it('the activity page: app cards, Apps and Inputs rows, an Inputs page, Tools kept off the apps', () => {
    const pages = templatePages(cfgInputs(), 'activity', { activity: 'watch' });
    const home = pages[0]!;
    expect(home.layout).toBe('fixed');
    const rows = home.widgets.filter((w) => w.shape === 'row');
    expect(rows.map((w) => w.label)).toEqual(['Apps', 'Inputs']);
    expect(home.widgets.filter((w) => w.shape !== 'row').every((w) => w.w === 6)).toBe(true);
    const inputs = pages.find((p) => p.title === 'Inputs')!;
    expect(inputs.widgets.map((w) => [w.label, w.icon, w.w])).toEqual([['HDMI 1', 'hdmi1', 12], ['HDMI 2', 'hdmi2', 12], ['HDMI 3', 'hdmi3', 12]]);
    expect(pages.flatMap((p) => p.widgets.map((w) => w.label))).not.toContain('Tools');
    expect(rows.find((w) => w.label === 'Inputs')?.nav).toBe(inputs.id);
  });

  it('Apps: cards up to 4, compact cards from 5, two a row', () => {
    const c4 = { ...cfgInputs(), devices: [{ id: 'box', name: 'Box', commands: many(4) }], activities: [{ id: 'watch', name: 'Box', devices: [{ device: 'box' }] }] } as unknown as Config;
    const a4 = templatePages(c4, 'activity', { activity: 'watch' }).find((p) => p.title === 'Apps')!;
    expect(a4.widgets.some((w) => w.compact)).toBe(false);
    const a6 = templatePages(cfgInputs(), 'activity', { activity: 'watch' }).find((p) => p.title === 'Apps')!;
    expect(a6.widgets).toHaveLength(6);
    expect(a6.widgets.every((w) => w.compact === true && w.w === 6)).toBe(true);
    expect(a6.widgets.map((w) => [w.x, w.y])).toEqual([[0, 0], [6, 0], [0, 1], [6, 1], [0, 2], [6, 2]]);
  });

  it('Home uses compact cards from 3 activities and keeps All Off before the rows that may page', () => {
    const c = { ...cfgInputs(), activities: ['a', 'b', 'c'].map((id) => ({ id, name: id.toUpperCase(), devices: [{ device: 'tv' }] })) } as unknown as Config;
    const home = templatePages(c, 'activities')[0]!;
    expect(home.widgets.filter((w) => w.w === 6).every((w) => w.compact === true)).toBe(true);
    expect(home.widgets.findIndex((w) => w.label === 'All Off')).toBe(3);
  });

  it('a Home Assistant app device gets the Apps layout: labelled cards up to 4, compact from 5', () => {
    const ha = (n: number): Config => ({ devices: [{ id: 'sa', name: 'Shield Apps', kind: 'ha', entity: 'media_player.x', icon: 'grid_apps', commands: many(n) }], activities: [], pages: [] }) as unknown as Config;
    const p4 = templatePages(ha(4), 'devices')[1]!;
    expect(p4.widgets.every((w) => w.w === 6 && !w.compact && w.icon_only === false)).toBe(true);
    const p6 = templatePages(ha(6), 'devices')[1]!;
    expect(p6.widgets.every((w) => w.w === 6 && w.compact === true)).toBe(true);
  });

  it('short captions drop trailing words; page titles fit the header', () => {
    expect(fitShort('Sleep Timer', (t) => t.length <= 6)).toBe('Sleep');
    expect(fitTitle('Speaker Power')).toBe('Speaker');
    expect(fitTitle('Living TV Inputs', 'Inputs')).toBe('Inputs');
  });

  it('an activity icon is a brand logo when the name has one, else a glyph', () => {
    expect(activityIcon('Shield TV')).toBe('nvidia');
    expect(activityIcon('Zzz Qqq')).toBeTruthy();
  });

  it('the checks flag a cut label and a row with no action', () => {
    const c = {
      devices: [],
      activities: [],
      pages: [{ id: 'p', layout: 'fixed', cols: 12, rows: 2, title: 'P', widgets: [
        { type: 'button', id: 'r', x: 0, y: 0, w: 12, h: 1, shape: 'row', label: 'Dead Row' },
        { type: 'button', id: 'c', x: 0, y: 1, w: 6, h: 1, label: 'A Very Long Card Label That Cannot Fit', icon: 'tv', action: { activity: 'off' } },
      ] }],
    } as unknown as Config;
    const kinds = studioChecks(c).map((k) => k.kind);
    expect(kinds).toContain('action');
    expect(kinds).toContain('label');
    expect(studioChecks({ ...c, pages: [] } as Config)).toEqual([]);
  });

  it("derived inputs can be renamed or hidden: they become the device's own inputs", () => {
    const d = cfgInputs().devices![0] as Device;
    expect(derivedInputs(d).map((i) => i.label)).toEqual(['HDMI 1', 'HDMI 2', 'HDMI 3']);
    const rows = derivedInputs(d).map((i) => ({ command: i.command, name: i.label, hidden: false }));
    rows[0]!.name = 'Shield';
    rows[2]!.hidden = true;
    const out = materializeInputs(cfgInputs(), 'tv', rows)!;
    expect(Object.keys((out.devices![0] as Device).inputs ?? {})).toEqual(['Shield', 'HDMI 2']);
    expect(derivedInputsNote(out.devices![0] as Device)).toBe('');
    expect(materializeInputs(cfgInputs(), 'tv', rows.map((r) => ({ ...r, name: 'Same' })))).toBeNull(); // two shown inputs share a name
    expect(materializeInputs(cfgInputs(), 'tv', rows.map((r) => ({ ...r, hidden: true })))).toBeNull();
  });
});
