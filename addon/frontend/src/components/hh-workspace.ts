import { LitElement, css, html } from 'lit';
import { customElement } from 'lit/decorators.js';

/**
 * Three-pane host for the visual editors: palette (left), canvas (centre), inspector (right).
 * Collapses to one column on phones (palette, canvas, inspector stacked).
 */
@customElement('hh-workspace')
export class HhWorkspace extends LitElement {
  static override styles = css`
    :host { display: grid; grid-template-columns: 240px minmax(0, 1fr) 280px; gap: 16px; align-items: start; min-width: 0; }
    :host > * { min-width: 0; }
    .pane { min-width: 0; }
    @media (max-width: 1100px) { :host { grid-template-columns: 220px minmax(0, 1fr); } .insp { grid-column: 1 / -1; } }
    @media (max-width: 720px) { :host { grid-template-columns: minmax(0, 1fr); } }
  `;
  protected override render() {
    return html`
      <aside class="pane" aria-label="Palette"><slot name="palette"></slot></aside>
      <section class="pane" aria-label="Canvas"><slot name="canvas"></slot></section>
      <aside class="pane insp" aria-label="Inspector"><slot name="inspector"></slot></aside>
    `;
  }
}
