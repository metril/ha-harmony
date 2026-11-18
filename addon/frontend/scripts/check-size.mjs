// Bundle-size budget: the built JS and CSS must stay under these gzip limits.
import { readdirSync, readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

// JS raised from 60 to 90 KiB for the remote studio (visual editor, 50 stock icons, Elite drawing); CSS is unchanged
const BUDGET = { js: 90 * 1024, css: 8 * 1024 }; // gzip bytes
const dir = new URL('../dist/assets/', import.meta.url);
let bad = false;
for (const ext of ['js', 'css']) {
  const files = readdirSync(dir).filter((f) => f.endsWith('.' + ext));
  if (files.length !== 1) {
    console.error(`expected exactly one .${ext}, found ${files.length}`);
    bad = true;
    continue;
  }
  const raw = readFileSync(new URL(files[0], dir));
  const gz = gzipSync(raw).length;
  const ok = gz <= BUDGET[ext];
  console.log(`${files[0]}: ${raw.length} B raw, ${gz} B gzip (budget ${BUDGET[ext]}) ${ok ? 'ok' : 'OVER'}`);
  if (!ok) bad = true;
}
process.exit(bad ? 1 : 0);
