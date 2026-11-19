import { LitElement, css, svg } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import type { V2Page } from '../lib/api';
import { pageGroup } from '../lib/page-renderer';
import { DPAD, GLASS, KEYS, MINI_VIEW, SCREEN, bodyPath, keyPath } from '../lib/remote-layout';

/** A small, read-only Elite drawing: optional screen page, and keys marked on / changed / missing / dim. */
@customElement('hh-mini-remote')
export class HhMiniRemote extends LitElement {
  static override styles = css`
    :host { display: inline-block; line-height: 0; }
    svg { width: 100%; height: auto; display: block; }
    .body { fill: #1b2024; stroke: #0a0d10; stroke-width: 3; }
    .glass { fill: #05080b; stroke: #1b2229; stroke-width: 2; }
    .cap { fill: #303840; stroke: #11161a; stroke-width: 1; }
    .c-red .cap { fill: #b8423a; } .c-green .cap { fill: #3e8e52; } .c-yellow .cap { fill: #bc9727; } .c-blue .cap { fill: #3b69b8; } .c-power_off_activity .cap { fill: #7a2b27; }
    .on .cap { fill: #20c8b0; stroke: #5fe3cf; stroke-width: 2; }
    .chg .cap { fill: #f8c040; stroke: #ffe08a; stroke-width: 2; }
    .miss .cap { fill: #1b2024; stroke: #f8c040; stroke-width: 2; stroke-dasharray: 5 3; }
    .dim .cap { opacity: 0.35; }
    .pulse .cap { stroke: #5fe3cf; stroke-width: 2.5; stroke-dasharray: 5 3; }
  `;
  @property({ attribute: false }) page: V2Page | null = null;
  /** key name -> mark: on | chg | miss | dim | pulse */
  @property({ attribute: false }) marks: Record<string, string> = {};
  @property() label = 'Harmony Elite remote';

  protected override render() {
    return svg`<svg viewBox="${MINI_VIEW.x} ${MINI_VIEW.y} ${MINI_VIEW.w} ${MINI_VIEW.h}" role="img" aria-label=${this.label}>
      <path class="body" d=${bodyPath()}></path>
      <rect class="glass" x=${GLASS.x} y=${GLASS.y} width=${GLASS.w} height=${GLASS.h} rx=${GLASS.r}></rect>
      <svg x=${SCREEN.x} y=${SCREEN.y} width=${SCREEN.w} height=${SCREEN.h} viewBox="0 0 240 320">
        ${this.page ? pageGroup(this.page, {}) : svg`<rect width="240" height="320" fill="#101820"></rect>`}
      </svg>
      <circle cx=${DPAD.x} cy=${DPAD.y} r=${DPAD.r + 3} fill="#12161a"></circle>
      ${KEYS.map((g) => svg`<g class="${this.marks[g.name] ?? ''} ${g.color ? `colour c-${g.name}` : ''}"><path class="cap" d=${keyPath(g)} ></path></g>`)}
    </svg>`;
  }
}
