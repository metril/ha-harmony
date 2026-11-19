// Bundle-size budget (gzip). The entry chunk is what every visit downloads: 90 KiB. The rarely opened
// routes (add-device wizard, activities, themes) are separate dynamic-import chunks, capped at 30 KiB each
// and 160 KiB for all JS together (raised from 135 when the theme editor and colour picker landed: about
// 18 KiB of lazily loaded code; the entry budget, which is what every visit pays, did not move). CSS is one file.
import { readdirSync, readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const KiB = 1024;
const BUDGET = { entry: 90 * KiB, chunk: 30 * KiB, total: 160 * KiB, css: 8 * KiB };
const dir = new URL('../dist/assets/', import.meta.url);
const index = readFileSync(new URL('../dist/index.html', import.meta.url), 'utf8');
let bad = false;
const check = (name, gz, max) => {
  const ok = gz <= max;
  console.log(`${name}: ${gz} B gzip (budget ${max}) ${ok ? 'ok' : 'OVER'}`);
  if (!ok) bad = true;
};

const js = readdirSync(dir).filter((f) => f.endsWith('.js'));
const css = readdirSync(dir).filter((f) => f.endsWith('.css'));
let total = 0;
for (const f of js) {
  const gz = gzipSync(readFileSync(new URL(f, dir))).length;
  total += gz;
  check(index.includes(f) ? `${f} (entry)` : f, gz, index.includes(f) ? BUDGET.entry : BUDGET.chunk);
}
check('all JS', total, BUDGET.total);
if (css.length !== 1) {
  console.error(`expected exactly one .css, found ${css.length}`);
  bad = true;
} else check(css[0], gzipSync(readFileSync(new URL(css[0], dir))).length, BUDGET.css);
process.exit(bad ? 1 : 0);
