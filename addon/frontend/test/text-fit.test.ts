import { describe, expect, it } from 'vitest';
import fixture from '../../../tests/vectors/text_fit.json';
import keyFixture from '../../../tests/vectors/key_names.json';
import { clip, fitLabel, labelBudget, textPx, type Font, type FitWidget } from '../src/lib/text-fit';
import { abbreviate, displayName } from '../src/lib/key-names';

// the same fixture runs in pytest (tests/py/test_text_fit.py)
describe('text-fit: shared vectors', () => {
  for (const c of fixture.cases) {
    it(c.name, () => {
      const page = { cols: c.cols, rows: c.rows };
      const w = c.widget as FitWidget;
      const b = labelBudget(page, w);
      if (c.expect === null) return expect(b).toBeNull();
      expect(b).toEqual({ font: c.expect.font, px: c.expect.px });
      expect(textPx(w.label, c.expect.font as Font)).toBe(c.expect.text_px);
      const k = clip(w.label, c.expect.font as Font, c.expect.px);
      expect(k).toEqual({ text: c.expect.shown, over: c.expect.over });
      expect(fitLabel(page, w)?.over ?? 0).toBe(c.expect.over);
    });
  }
  for (const c of fixture.clip) it(`clip ${c.s}`, () => expect(clip(c.s, c.font as Font, c.px)).toEqual({ text: c.shown, over: c.over }));
  it.each(keyFixture.display as [string, string][])('displayName %s -> %s', (a, b) => expect(displayName(a)).toBe(b));
  it.each(keyFixture.abbreviate as [string, string][])('abbreviate %s -> %s', (a, b) => expect(abbreviate(a)).toBe(b));
  it('counts an unknown glyph as "?"', () => expect(textPx('中', 'b13')).toBe(textPx('?', 'b13')));
});
