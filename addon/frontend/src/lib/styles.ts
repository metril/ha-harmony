import { css } from 'lit';

// Shared component styles, adopted by every component through `static styles`.
export const shared = css`
  *, *::before, *::after { box-sizing: border-box; }
  [hidden] { display: none !important; }
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
  .sic { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; flex: none; }
  .chip { display: inline-flex; align-items: center; gap: 4px; min-height: 24px; padding: 0 8px; border-radius: 12px; font-size: 12px; font-weight: 600; background: var(--sf2); color: var(--tx2); white-space: nowrap; }
  /* a list of pills as a grid: equal cells and heights, the label truncated with an ellipsis (give each a title) */
  .cells { display: grid; grid-template-columns: repeat(auto-fill, minmax(8.25rem, 1fr)); gap: 6px; grid-auto-flow: dense; }
  .cells { container-type: inline-size; }
  .cells > [data-span="2"] { grid-column: span 2; }
  .cells > [data-span="full"] { grid-column: 1 / -1; }
  /* a left|right pair always shares one row: two equal halves, the whole row when a label needs more than a half */
  .cells > .pair { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; grid-column: span 2; overflow: visible; height: 30px; }
  .cells > .pair[data-span="full"], .cells > .pair[data-span="stack"] { grid-column: 1 / -1; }
  .cells > .pair[data-span="stack"] { grid-template-columns: 1fr; height: auto; }
  @container (max-width: 17rem) { .cells > .pair { grid-column: 1 / -1; } }
  .cells > .pair > * { box-sizing: border-box; height: 30px; min-width: 0; width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; text-align: left; justify-content: flex-start; }
  .cells > .pair > .chip2 { justify-content: center; text-align: center; }
  .cells > * { box-sizing: border-box; height: 30px; min-width: 0; width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; text-align: left; justify-content: flex-start; }
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
  .problems { border-color: var(--bad); background: var(--bads); }
  .problems h2 { color: var(--bad); }
  .problems ul { margin: 0; padding: 0; list-style: none; display: grid; gap: 6px; }
  .problems li { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
  .problems li span { overflow-wrap: anywhere; min-width: 0; flex: 1 1 200px; }
  .meter { display: grid; gap: 3px; min-width: 110px; font-size: 11px; color: var(--tx3); }
  .meter b { color: var(--tx2); font-weight: 600; }
  .meter em.over { display: block; color: var(--bad); font-style: normal; font-weight: 600; }
  .bar { height: 6px; border-radius: 3px; background: var(--sf3); overflow: hidden; }
  .bar i { display: block; height: 100%; background: var(--acc); border-radius: 3px; }
  .bar i.warn { background: var(--ir); }
  .bar i.full { background: var(--bad); }
  .vh { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
  @media (max-width: 720px) { .btn { min-height: 44px; } .btn.sm { min-height: 44px; } .btn.icon { min-width: 44px; } }
`;

/** The chip look of the palette (Devices tab commands, Home Assistant entity rows). */
export const chipStyles = css`
  .chip2 { display: flex; align-items: center; gap: 6px; box-sizing: border-box; height: 30px; min-width: 0; width: 100%; padding: 0 10px; border-radius: 15px; text-align: left; border: 1px solid var(--line); background: var(--sf2); color: var(--tx); font-size: 12.5px; cursor: grab; touch-action: pan-y; user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
  .cells > .chip2 { justify-content: center; text-align: center; }
  .chip2:hover { background: var(--sf3); }
  .chip2 > span { flex: 0 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .chip2.used { border-color: var(--acc); box-shadow: inset 0 0 0 1px var(--acc); }
  .chip2[aria-pressed="true"] { background: var(--acc); border-color: var(--acc); color: var(--onacc); }
  .chip2 .sic { width: 15px; height: 15px; flex: none; }
  @media (max-width: 720px) { .chip2 { height: 36px; } }
`;
