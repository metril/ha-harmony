import { css } from 'lit';

// Shared component styles, adopted by every component through `static styles`.
export const shared = css`
  :host { box-sizing: border-box; }
  *, *::before, *::after { box-sizing: inherit; }
  .ic { width: 18px; height: 18px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; flex: none; }
  button, a, input, select, textarea, summary { font: inherit; }
  :focus-visible { outline: 2px solid var(--focus); outline-offset: 2px; }
  .btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; height: 36px; padding: 0 14px; border-radius: var(--r1); border: 1px solid var(--line); background: var(--sf); color: var(--tx); font-weight: 500; font-size: 13px; white-space: nowrap; cursor: pointer; text-decoration: none; transition: background 120ms ease-out; }
  .btn:hover { background: var(--sf2); }
  .btn.pri { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
  .btn.pri:hover { filter: brightness(1.08); }
  .btn.ghost { border-color: transparent; background: transparent; }
  .btn.ghost:hover { background: var(--sf2); }
  .btn.danger { color: var(--bad); }
  .btn.sm { height: 28px; padding: 0 10px; font-size: 12px; }
  .btn.icon { width: 36px; padding: 0; }
  .btn[aria-disabled="true"], .btn:disabled { opacity: 0.55; cursor: not-allowed; }
  .btn[aria-disabled="true"]:hover { background: var(--sf); }
  .btn.pri[aria-disabled="true"]:hover { background: var(--acc); filter: none; }
  .chip { display: inline-flex; align-items: center; gap: 4px; min-height: 24px; padding: 0 8px; border-radius: 12px; font-size: 12px; font-weight: 600; background: var(--sf2); color: var(--tx2); white-space: nowrap; }
  .chip.ok { background: var(--oks); color: var(--ok); }
  .chip.bad { background: var(--bads); color: var(--bad); }
  .chip.acc { background: var(--accs); color: var(--tx); }
  .chip.ir { background: var(--irs); color: var(--ir); }
  .chip .ic { width: 14px; height: 14px; }
  .card { background: var(--sf); border: 1px solid var(--line); border-radius: var(--r2); padding: 16px; min-width: 0; }
  .card h2, .card h3 { margin: 0 0 12px; font-size: 14px; font-weight: 650; display: flex; align-items: center; gap: 8px; }
  h1 { font-size: 20px; margin: 0; font-weight: 650; }
  .muted { color: var(--tx3); font-size: 12px; }
  .mono { font-family: var(--mono); font-size: 12.5px; }
  .banner { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; padding: 10px 14px; border-radius: var(--r2); border: 1px solid var(--line); background: var(--sf); }
  .banner.bad { background: var(--bads); border-color: var(--bad); color: var(--tx); }
  .banner.ir { background: var(--irs); border-color: var(--ir); }
  .sp { flex: 1; }
  .meter { display: grid; gap: 3px; min-width: 110px; font-size: 11px; color: var(--tx3); }
  .meter b { color: var(--tx2); font-weight: 600; }
  .bar { height: 6px; border-radius: 3px; background: var(--sf3); overflow: hidden; }
  .bar i { display: block; height: 100%; background: var(--acc); border-radius: 3px; }
  .bar i.warn { background: var(--ir); }
  .bar i.full { background: var(--bad); }
  .vh { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
  @media (max-width: 720px) { .btn { min-height: 44px; } .btn.sm { min-height: 44px; } .btn.icon { min-width: 44px; } }
`;
