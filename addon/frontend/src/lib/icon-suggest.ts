// Which stock icon suits a command or label, and why. Twin of harmony_core.page_build.icon_for_text (same word split and
// the same rule: an icon named exactly like the text beats any keyword overlap), plus the reason the picker shows.
import { displayName } from './key-names';
import { STOCK_ICONS, type StockIcon } from './stock-icons';

/** Glyph ids from this one up are proposed: the picker marks them "new" until the firmware that draws them ships. */
export const NEW_ICON_FROM = 51;
export const isNewIcon = (i: StockIcon): boolean => i.id >= NEW_ICON_FROM;

const EXPAND: Record<string, string> = { vol: 'volume', ch: 'channel', chan: 'channel', pwr: 'power', prev: 'previous' };
/** Lower-case words of a command or label: camelCase and snake_case split ("DirectionUp" -> direction, up). */
export const words = (text: string): string[] =>
  (text.replace(/([a-z0-9])(?=[A-Z])|([A-Z])(?=[A-Z][a-z])/g, '$1$2 ').replace(/[_\s]+/g, ' ').toLowerCase().match(/[a-z0-9+-]+/g) ?? []);

export interface Suggestion {
  icon: StockIcon;
  score: number;
  /** why it matched, for the picker ("name Up", "keyword volume", "device icon") */
  reason: string;
}
interface Src { text: string; exact: number; kw: number; tag: string }

/** Ranked icons: command-name words first, then the label, then the device's own icon. Empty when nothing matches. */
export function suggestIcons(command: string, label = '', deviceIcon = ''): Suggestion[] {
  const srcs: Src[] = [];
  if (command) srcs.push({ text: command, exact: 400, kw: 30, tag: 'command' }, { text: displayName(command), exact: 400, kw: 30, tag: 'command' });
  if (label) srcs.push({ text: label, exact: 300, kw: 10, tag: 'label' });
  const out: Suggestion[] = [];
  for (const icon of STOCK_ICONS) {
    let score = 0;
    let reason = '';
    for (const s of srcs) {
      const ws = words(s.text);
      const toks = new Set<string>();
      for (const t of ws) {
        toks.add(t);
        toks.add(EXPAND[t] ?? t);
        if (t.length > 3 && t.endsWith('s')) toks.add(t.slice(0, -1));
      }
      const exact = ws.join('_') === icon.name;
      const hits = icon.keywords.filter((k) => toks.has(k));
      const sc = (exact ? s.exact : 0) + hits.length * s.kw;
      if (sc > 0 && !reason) reason = exact ? `${s.tag} name "${displayName(icon.name)}"` : `keyword "${hits[0]}" in the ${s.tag}`;
      score += sc;
    }
    if (deviceIcon && icon.name === deviceIcon) {
      score += 5;
      reason ||= 'the device icon';
    }
    if (score > 0) out.push({ icon, score, reason });
  }
  return out.sort((a, b) => b.score - a.score || a.icon.id - b.icon.id);
}
/** The best icon name for a command, or '' when none matches. */
export const bestIcon = (command: string, label = ''): string => suggestIcons(command, label)[0]?.icon.name ?? '';
