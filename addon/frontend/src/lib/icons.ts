// Inline SVG icons on a 24-unit grid, 1.8 stroke, currentColor. Icons always sit beside text,
// except icon-only buttons, which carry an aria-label.
import { svg, type TemplateResult } from 'lit';

const PATHS: Record<string, string> = {
  overview: 'M3 12l9-8 9 8M5 10v10h14V10',
  devices: 'M4 5h16v11H4zM9 20h6M12 16v4',
  activities: 'M5 4l14 8-14 8z',
  keys: 'M8 3h8a2 2 0 012 2v14a2 2 0 01-2 2H8a2 2 0 01-2-2V5a2 2 0 012-2zM12 8v.01M12 13v.01',
  pages: 'M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z',
  ir: 'M4 8a12 12 0 0116 0M7 12a7 7 0 0110 0M10 16a2.5 2.5 0 014 0M12 20v.01',
  history: 'M3 12a9 9 0 109-9 9 9 0 00-6.4 2.6L3 8M3 3v5h5M12 7v5l3 2',
  menu: 'M4 6h16M4 12h16M4 18h16',
  upload: 'M12 16V4M7 9l5-5 5 5M4 20h16',
  download: 'M12 4v12M7 11l5 5 5-5M4 20h16',
  undo: 'M9 14L4 9l5-5M4 9h11a5 5 0 010 10h-3',
  check: 'M5 12l5 5 9-10',
  alert: 'M12 4l9 16H3zM12 10v4M12 17v.01',
  x: 'M6 6l12 12M18 6L6 18',
  more: 'M5 12v.01M12 12v.01M19 12v.01',
  hub: 'M4 15h16v5H4zM7 18v.01M12 12a6 6 0 00-6 0M16 9a10 10 0 00-8 0',
  clock: 'M12 7v5l3 2M12 3a9 9 0 100 18 9 9 0 000-18z',
  plus: 'M12 5v14M5 12h14',
};

export function icon(name: string, cls = 'ic'): TemplateResult {
  return svg`<svg class=${cls} viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d=${PATHS[name] ?? ''}></path></svg>`;
}
