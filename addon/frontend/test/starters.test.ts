import { describe, expect, it } from 'vitest';
import type { Config } from '../src/lib/model';
import { isHardKey, labelMeter, pageOverflow, templatePages } from '../src/lib/studio-model';
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

  it('the device starter is one nav row per device plus a 3-column grid each, labelled by command', () => {
    const pages = templatePages(cfg(), 'devices');
    expect(pages.map((p) => p.id)).toEqual(['devices', 'dev_shield', 'dev_tv']);
    expect(pages[0]?.widgets.map((w) => [w.label, w.nav])).toEqual([['Shield TV', 'dev_shield'], ['LG TV', 'dev_tv']]);
    const tv = pages[2]!;
    expect(tv.cols).toBe(3);
    const labels = tv.widgets.map((w) => w.label);
    expect(labels).toContain('HDMI 1');
    expect(labels).toContain('Power On');
    expect(labels).not.toContain('Menu'); // a hard key
    expect(labels.some((l) => /^lg|input hdmi/i.test(l ?? ''))).toBe(false);
  });

  it('no starter label overflows its cell', () => {
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
    const s = suggestIcons('InputHdmi1', 'HDMI 1');
    expect(s[0]?.icon.name).toBe('input');
    expect(s[0]?.reason).toContain('keyword');
  });
});
