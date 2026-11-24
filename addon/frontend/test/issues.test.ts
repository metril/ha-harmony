import { describe, expect, it } from 'vitest';
import { collectIssues, issueText, widgetIssues, type Issue } from '../src/lib/issues';
import type { StudioCheck } from '../src/lib/studio-model';

const label: StudioCheck = { kind: 'label', page: 0, index: 2, msg: 'Main: "Power Toggle" is 4 px too wide; use "Power"', short: 'Power' };
const glyph: StudioCheck = { kind: 'glyph', msg: 'Device "TV" has no glyph; pick an icon for it' };

describe('issues', () => {
  it('orders errors, add-on warnings, then checks', () => {
    const i = collectIssues([{ path: ['keys', 0], msg: 'bad key' }], [{ path: ['pages', 0], msg: 'warn' }], [label, glyph], null);
    expect(i.map((x) => x.level)).toEqual(['error', 'warning', 'warning', 'warning']);
    expect(i.map((x) => x.text)).toEqual(['bad key', 'warn', issueText(label), issueText(glyph)]);
    expect(i[0]?.target).toEqual({ kind: 'route', route: 'keys' });
  });
  it('errors with a widget or key path target it; others fall back to the route', () => {
    const [a, b, c] = collectIssues([{ path: ['keys', 'menu'], msg: 'k' }, { path: ['pages', 0, 'widgets', 1, 'x'], msg: 'w' }, { path: ['devices', 0], msg: 'd' }], [], [], null);
    expect(a?.target).toEqual({ kind: 'error', path: ['keys', 'menu'] });
    expect(b?.target).toEqual({ kind: 'error', path: ['pages', 0, 'widgets', 1, 'x'] });
    expect(c?.target?.kind).toBe('route');
  });
  it('label checks carry a fix; glyph checks have no target', () => {
    const [l, g] = collectIssues([], [], [label, glyph], null);
    expect(l?.fix).toEqual({ label: 'Use "Power"', value: 'Power' });
    expect(l?.target).toEqual({ kind: 'widget', page: 0, index: 2 });
    expect(g?.target).toBeUndefined();
    expect(g?.fix).toBeUndefined();
  });
  it('widgetIssues lets an error win over a warning', () => {
    const w: Issue = { level: 'warning', text: 'w', target: { kind: 'widget', page: 0, index: 1 } };
    const e: Issue = { level: 'error', text: 'e', target: { kind: 'widget', page: 0, index: 1 } };
    const other: Issue = { level: 'warning', text: 'o', target: { kind: 'widget', page: 1, index: 1 } };
    const m = widgetIssues([w, e, other], 0);
    expect(m.size).toBe(1);
    expect(m.get(1)?.text).toBe('e');
  });
});
