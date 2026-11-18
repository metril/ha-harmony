import { noChange } from 'lit';
import { Directive, PartType, directive, type PartInfo, type ElementPart, type AttributePart } from 'lit/directive.js';

/**
 * CSP-safe replacement for Lit's stock style directive, whose first render returns a CSS string
 * that Lit writes with setAttribute('style', ...); `style-src 'self'` blocks that (verified in Chromium).
 * This directive only ever uses CSSOM (`style.setProperty`), which the CSP allows, and never
 * writes the attribute. Use as `style=${setStyle({ width: '40%' })}`, unquoted.
 */
class SetStyleDirective extends Directive {
  private prev = new Set<string>();
  constructor(info: PartInfo) {
    super(info);
    if (info.type !== PartType.ATTRIBUTE || info.name !== 'style') throw new Error('setStyle must be bound to the style attribute');
  }
  render(info: Record<string, string | number | null | undefined>) {
    void info;
    return noChange;
  }
  override update(part: AttributePart | ElementPart, [info]: [Record<string, string | number | null | undefined>]) {
    const style = (part.element as HTMLElement).style;
    for (const name of this.prev) {
      if (info[name] == null) {
        style.removeProperty(name);
        this.prev.delete(name);
      }
    }
    for (const [name, value] of Object.entries(info)) {
      if (value == null) continue;
      style.setProperty(name, String(value));
      this.prev.add(name);
    }
    return noChange;
  }
}
export const setStyle = directive(SetStyleDirective);
