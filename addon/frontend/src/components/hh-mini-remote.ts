import { LitElement, css, nothing, svg } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import type { V2Page } from '../lib/api';
import { pageGroup } from '../lib/page-renderer';
import type { RenderTheme } from '../lib/theme-model';
import { GLASS, JOINED, PLATED, PLATES, KEYS, MINI_VIEW, SCREEN, bodyPath, gripPath, keyPath, tintPath } from '../lib/remote-layout';

/** A small, read-only Elite drawing: optional screen page, and keys marked on / changed / missing / dim. */
@customElement('hh-mini-remote')
export class HhMiniRemote extends LitElement {
  static override styles = css`
    :host { display: inline-block; line-height: 0; }
    svg { width: 100%; height: auto; display: block; }
    .body { fill: #0d0f11; stroke: #000; stroke-width: 3; }
    .grip { fill: #272a2e; }
    .glass { fill: none; stroke: none; }
    .cap { fill: #565d65; stroke: #15181b; stroke-width: 1; }
    .g-colour .cap { fill: #2c3135; }
    .g-strip .cap { fill: #030405; stroke: #e8edf0; stroke-width: 2; }
    .tint { fill: none; stroke-width: 3; }
    .jn .cap { fill: #565d65; stroke: #565d65; stroke-width: 0.7; }
    .plate { fill: #565d65; stroke: #565d65; stroke-width: 0.7; }
    .jp .cap { fill: transparent; stroke: none; }
    .on .cap { stroke: #3ee8cf; stroke-width: 3; }
    .chg .cap { stroke: #f8c040; stroke-width: 3; }
    .miss .cap { fill: #1b2024; stroke: #f8c040; stroke-width: 2; stroke-dasharray: 5 3; }
    .dim .cap { opacity: 0.35; }
    .pulse .cap { stroke: #5fe3cf; stroke-width: 2.5; stroke-dasharray: 5 3; }
  `;
  @property({ attribute: false }) page: V2Page | null = null;
  /** key name -> mark: on | chg | miss | dim | pulse */
  @property({ attribute: false }) marks: Record<string, string> = {};
  /** the theme to draw the page in (default: the preset the page names) */
  @property({ attribute: false }) theme?: RenderTheme;
  @property() label = 'Harmony Elite remote';

  protected override render() {
    return svg`<svg viewBox="${MINI_VIEW.x} ${MINI_VIEW.y} ${MINI_VIEW.w} ${MINI_VIEW.h}" role="img" aria-label=${this.label}>
      <path class="body" d=${bodyPath()}></path>
      <rect class="glass" x=${GLASS.x} y=${GLASS.y} width=${GLASS.w} height=${GLASS.h} rx=${GLASS.r}></rect>
      <svg x=${SCREEN.x} y=${SCREEN.y} width=${SCREEN.w} height=${SCREEN.h} viewBox="0 0 240 320">
        ${this.page ? pageGroup(this.page, this.theme ? { theme: this.theme } : {}) : svg`<rect width="240" height="320" fill="#101820"></rect>`}
      </svg>
      <path class="grip" d=${gripPath()}></path>
      ${PLATES.map((d) => svg`<path class="plate" d=${d}></path>`)}
      ${KEYS.map((g) => svg`<g class="${this.marks[g.name] ?? ''} g-${g.group} ${JOINED.has(g.name) ? 'jn' : ''} ${PLATED.has(g.name) ? 'jp' : ''}"><path class="cap" d=${keyPath(g)}></path>${g.color ? svg`<path class="tint" d=${tintPath(g)} stroke=${g.color}></path>` : nothing}</g>`)}
    </svg>`;
  }
}
