import { describe, expect, it } from 'vitest';
import type { Config } from '../src/lib/model';
import { diffConfig } from '../src/lib/diff';
import { pageBytes } from '../src/lib/studio-model';
import { activeName, activeTheme, colourBytes, degradeText, hexOf, parseHex, themeNamed } from '../src/lib/theme-model';
import {
  createTheme, deleteTheme, duplicateTheme, exportTheme, fixAll, importTheme, overridesToken, pushRecent, renameTheme, resetToken, selectTheme,
  setPaletteFill, setToken, themeProblems, themeUsage, uniqueThemeName,
} from '../src/lib/theme-edit';
import { snap565 } from '../src/lib/theme-math';

const base = (): Config => ({ pages: [{ id: 'p', widgets: [{ type: 'button', id: 'a', label: 'A', action: { none: true } }] }] });

describe('theme editing', () => {
  it('a config without themes is untouched by selecting the default', () => {
    const c = base();
    expect(JSON.stringify(selectTheme(c, 'default'))).toBe(JSON.stringify(c));
    expect(activeName(c)).toBe('default');
    expect(selectTheme(c, 'midnight').theme).toBe('midnight');
  });
  it('duplicate a preset: extends it, a free name, no overrides yet', () => {
    const { config, name } = duplicateTheme(base(), 'default');
    expect(name).toBe('my_default');
    expect(config.themes).toEqual({ my_default: { extends: 'default' } });
    expect(duplicateTheme(config, 'default').name).toBe('my_default_2');
    expect(uniqueThemeName(config, 'My Default!')).toBe('my_default_2');
  });
  it('tokens override the parent; the same value as the parent is no override; reset removes it', () => {
    let c = duplicateTheme(base(), 'midnight').config;
    c = setToken(c, 'my_midnight', 'acc', '#ff8a3d');
    expect(c.themes?.['my_midnight']?.tokens).toEqual({ acc: '#FF8A3D' });
    expect(themeNamed(c, 'my_midnight').theme.tokens.acc).toBe(snap565(0xff8a3d));
    expect(overridesToken(c, 'my_midnight', 'acc')).toBe(true);
    expect(setToken(c, 'my_midnight', 'acc', hexOf(0x7888f8)).themes?.['my_midnight']?.tokens).toBeUndefined();
    expect(resetToken(c, 'my_midnight', 'acc').themes?.['my_midnight']?.tokens).toBeUndefined();
  });
  it('inheritance: a theme extending a user theme sees its changes; YAML-style upper-case tokens work', () => {
    const c: Config = { themes: { a: { extends: 'midnight', tokens: { ACC: '#FF0000' } }, b: { extends: 'a', palette: { '3': { fill: '#2E7D32' } } } }, theme: 'b' };
    const t = activeTheme(c).theme;
    expect(t.tokens.acc).toBe(snap565(0xff0000));
    expect(t.tokens.bg).toBe(0);
    expect(t.base).toBe('midnight');
    expect(t.swatches[2]?.fill).toBe(snap565(0x2e7d32));
    expect(setPaletteFill(c, 'b', 2, hexOf(0xe0b040)).themes?.['b']?.palette).toBeUndefined(); // the parent's value is no override
    expect(setPaletteFill(c, 'b', 3, '#2E7D32').themes?.['b']?.palette).toMatchObject({ '3': { fill: '#2E7D32' }, '4': { fill: '#2E7D32' } });
  });
  it('a broken theme falls back to the default look and says why', () => {
    const r = themeNamed({ themes: { x: { extends: 'nope' } } }, 'x');
    expect(r.error).toMatch(/unknown theme/);
    expect(r.theme.tokens.bg).toBe(0x101820);
  });
  it('delete: blocked while another theme extends it; the active one switches to its parent; the last one removes `themes`', () => {
    let c = duplicateTheme(base(), 'daylight').config;
    c = selectTheme(c, 'my_daylight');
    expect(themeUsage(c, 'my_daylight')).toEqual({ active: true, children: [] });
    const gone = deleteTheme(c, 'my_daylight');
    expect(gone.themes).toBeUndefined();
    expect(gone.theme).toBe('daylight');
    const two = createTheme(c, 'kid', 'my_daylight');
    expect(themeUsage(two, 'my_daylight').children).toEqual(['kid']);
    expect(deleteTheme(selectTheme(duplicateTheme(base(), 'default').config, 'my_default'), 'my_default').theme).toBeUndefined();
  });
  it('rename updates the active theme and every extends', () => {
    let c = selectTheme(createTheme(duplicateTheme(base(), 'default').config, 'kid', 'my_default'), 'my_default');
    c = renameTheme(c, 'my_default', 'dusk');
    expect(c.theme).toBe('dusk');
    expect(Object.keys(c.themes ?? {})).toEqual(['dusk', 'kid']);
    expect(c.themes?.['kid']?.extends).toBe('dusk');
  });
  it('problems and fixes: Fix all clears them', () => {
    let c = duplicateTheme(base(), 'midnight').config;
    c = setToken(c, 'my_midnight', 'tx', '#404848');
    const probs = themeProblems(themeNamed(c, 'my_midnight').theme);
    expect(probs.length).toBeGreaterThan(1);
    expect(probs[0]?.title).toMatch(/Main text on/);
    const fixed = fixAll(c, 'my_midnight');
    expect(themeProblems(themeNamed(fixed, 'my_midnight').theme)).toEqual([]);
  });
});

describe('import and export', () => {
  it('round trips a flattened theme; a name clash becomes name_2', () => {
    let c = duplicateTheme(base(), 'midnight').config;
    c = setToken(c, 'my_midnight', 'acc', '#FF8A3D');
    c = setPaletteFill(c, 'my_midnight', 0, '#CC2222');
    const text = exportTheme(c, 'my_midnight');
    const o = JSON.parse(text) as Record<string, unknown>;
    expect(o).toMatchObject({ format: 'harmony-theme', name: 'my_midnight', extends: 'midnight', tokens: { acc: hexOf(snap565(0xff8a3d)) }, palette: { '1': { fill: hexOf(snap565(0xcc2222)) } } });
    const { config, name } = importTheme(c, text);
    expect(name).toBe('my_midnight_2');
    expect(themeNamed(config, name).theme.tokens.acc).toBe(snap565(0xff8a3d));
    expect(themeNamed(config, name).theme.swatches[0]?.fill).toBe(snap565(0xcc2222));
    expect(importTheme(base(), text).name).toBe('my_midnight');
  });
  it('flattens a chain of user themes', () => {
    const c: Config = { themes: { a: { extends: 'daylight', tokens: { acc: '#112233' } }, b: { extends: 'a', tokens: { bad: '#AA0000' } } } };
    const o = JSON.parse(exportTheme(c, 'b')) as { extends: string; tokens: Record<string, string> };
    expect(o.extends).toBe('daylight');
    expect(Object.keys(o.tokens).sort()).toEqual(['acc', 'bad']);
  });
  it('rejects other files in plain words', () => {
    expect(() => importTheme(base(), 'not json')).toThrow(/not valid JSON/);
    expect(() => importTheme(base(), '{"a":1}')).toThrow(/not a Harmony theme/);
    expect(() => importTheme(base(), '{"format":"harmony-theme","extends":"zzz"}')).toThrow(/not a preset/);
    expect(() => importTheme(base(), '{"format":"harmony-theme","extends":"default","tokens":{"acc":"red"}}')).toThrow(/not valid/);
  });
});

describe('picker logic and bookkeeping', () => {
  it('parses hex forms', () => {
    expect(parseHex('#3355ff')).toBe('#3355FF');
    expect(parseHex('35f')).toBe('#3355FF');
    expect(parseHex('#12345')).toBeNull();
  });
  it('keeps the last 8 colours, newest first, no duplicates', () => {
    let r: string[] = [];
    for (let i = 0; i < 10; i++) r = pushRecent(r, `#00000${i}`);
    expect(r).toHaveLength(8);
    expect(r[0]).toBe('#000009');
    expect(pushRecent(r, '#000005')[0]).toBe('#000005');
    expect(pushRecent(r, '#000005')).toHaveLength(8);
  });
  it('colour bytes: palette 1, each free colour 2 more, counted in the page size', () => {
    expect(colourBytes({})).toBe(0);
    expect(colourBytes({ color: 'red' })).toBe(1);
    expect(colourBytes({ color: '#112233' })).toBe(3);
    expect(colourBytes({ color: '#112233', on_color: '#445566' })).toBe(5);
    const p = { id: 'p', widgets: [{ type: 'button', label: 'A' }] };
    const plain = pageBytes(p);
    expect(pageBytes({ ...p, widgets: [{ type: 'button', label: 'A', color: '#112233' }] })).toBe(plain + 3);
  });
  it('degrade notice wording', () => {
    expect(degradeText([])).toBeNull();
    expect(degradeText(['page x: custom colours snapped to the palette'])).toMatch(/palette colours only/);
    expect(degradeText(['theme t not shown: the remote firmware has no colour support'])).toMatch(/no colour support/);
  });
  it('the diff names theme changes', () => {
    const a = base();
    const b = selectTheme(duplicateTheme(a, 'default').config, 'my_default');
    const rows = diffConfig(a, b).map((r) => `${r.kind} ${r.area} ${r.label}`);
    expect(rows).toContain('added Theme my_default');
    expect(rows).toContain('changed Active theme my_default');
    expect(diffConfig(a, base())).toEqual([]);
  });
});
