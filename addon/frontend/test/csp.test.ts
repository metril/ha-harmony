import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const SRC = join(process.cwd(), 'src');
const DIST = join(process.cwd(), 'dist', 'assets');

function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((f) => {
    const p = join(dir, f);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
}
const sources = walk(SRC).filter((f) => f.endsWith('.ts'));

describe('CSP-safe rendering rules', () => {
  it('sources never use innerHTML, unsafeHTML, <style> tags or style attributes other than styleMap', () => {
    for (const f of sources) {
      const t = readFileSync(f, 'utf8');
      expect(t, f).not.toMatch(/innerHTML|outerHTML|insertAdjacentHTML|unsafeHTML|unsafe-html|unsafeSVG/);
      expect(t, f).not.toMatch(/<style[\s>]/);
      for (const m of t.matchAll(/\sstyle\s*=\s*(\S{1,12})/g)) expect(m[1], f).toMatch(/^\$\{styleMap\(/);
    }
  });
  it('sources make no external requests', () => {
    for (const f of sources) expect(readFileSync(f, 'utf8'), f).not.toMatch(/https?:\/\/(?!ha\.example)(?!www\.w3\.org)/);
  });

  const built = existsSync(DIST);
  it.skipIf(!built)('the built bundle has the unsafe directives tree-shaken out and no external URLs', () => {
    const files = readdirSync(DIST);
    expect(files.filter((f) => f.endsWith('.js'))).toHaveLength(1);
    expect(files.filter((f) => f.endsWith('.css'))).toHaveLength(1);
    for (const f of files) {
      const t = readFileSync(join(DIST, f), 'utf8');
      expect(t, f).not.toMatch(/unsafeHTML|unsafeSVG|unsafe-html/);
      expect(t, f).not.toMatch(/https?:\/\/(?!www\.w3\.org|lit\.dev)/);
      expect(t, f).not.toMatch(/@import|[^.\w]url\(/);
    }
  });
});
