import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { SMALL_WORDS } from '../src/lib/key-names';

// Every fixed control label (button, tab, link, heading, summary, field label, option, column header) is Title Case: no all-lower-case word
// except the small joining words, and no underscore. Sentences (help text, banners, toasts) are not labels and are not checked.
const SRC = join(__dirname, '..', 'src');
const walk = (d: string): string[] => readdirSync(d).flatMap((n) => (statSync(join(d, n)).isDirectory() ? walk(join(d, n)) : n.endsWith('.ts') ? [join(d, n)] : []));
const TAG = /<(button|a|h[1-4]|summary|label|th|option|legend)\b[^>]*>\s*([A-Z+←→][A-Za-z&;' ,()/…+?!-]{1,60}?)\s*<\//g;
// labels that are deliberately not Title Case: units in parentheses, a quoted example, a trailing hint
const ALLOW = new Set(['Settle Time (ms)']);
const bad = (label: string): string | null => {
  const t = label.replace(/&amp;/g, '&');
  if (ALLOW.has(t) || t.includes('_')) return ALLOW.has(t) ? null : 'underscore';
  const words = t.split(/\s+/);
  if (words.length > 6) return null; // a sentence, not a label
  for (const [i, w] of words.entries()) {
    const core = w.replace(/^[^A-Za-z]+|[^A-Za-z]+$/g, '');
    if (/^[a-z]+$/.test(core) && !(i > 0 && SMALL_WORDS.has(core)) && !/^(ms|s)$/.test(core)) return `lower-case word "${core}"`;
  }
  return null;
};

describe('control labels', () => {
  it('are Title Case and carry no raw ids', () => {
    const found: string[] = [];
    for (const f of walk(SRC)) {
      const src = readFileSync(f, 'utf8');
      for (const m of src.matchAll(TAG)) {
        const why = bad(m[2] as string);
        if (why) found.push(`${f.slice(SRC.length + 1)}: "${m[2]}" (${why})`);
      }
    }
    expect(found).toEqual([]);
  });
  it('the rule itself', () => {
    expect(bad('Add Device')).toBeNull();
    expect(bad('Open in the Studio')).toBeNull();
    expect(bad('Add device')).toMatch(/device/);
    expect(bad('power_on')).toBe('underscore');
  });
});
