import { css, html, nothing } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import type { PreviewPage } from '../lib/api';
import { changedPages, changedWidgetIndexes } from '../lib/diff';
import type { Page } from '../lib/model';
import { pageSvg, widgetRect, type Rect } from '../lib/page-renderer';
import { activeTheme, type RenderTheme } from '../lib/theme-model';
import { shared } from '../lib/styles';
import { wireMap } from '../lib/studio-model';
import { StoreElement } from './base';

const KIND_LABEL = { added: 'Added', changed: 'Changed', removed: 'Removed' } as const;

/** Before / after thumbnails of every page the draft changes; changed widgets are outlined in amber. */
@customElement('hh-page-diff')
export class HhPageDiff extends StoreElement {
  static override styles = [
    shared,
    css`
      :host { display: block; }
      .pages { display: grid; gap: 14px; }
      .pg { display: grid; grid-template-columns: auto auto minmax(0, 1fr); gap: 12px; align-items: center; }
      .pg figure { margin: 0; display: grid; gap: 4px; justify-items: center; font-size: 11px; color: var(--tx2); }
      .pg svg { border-radius: 8px; box-shadow: 0 0 0 1px var(--line); display: block; }
      .pg figure.none { width: 72px; }
      .legend { font-size: 12px; color: var(--tx2); display: flex; gap: 6px; align-items: center; margin: 0 0 8px; }
      .legend i { width: 14px; height: 10px; border: 2px solid #f8c040; border-radius: 3px; display: inline-block; }
      @media (max-width: 760px) { .pg { grid-template-columns: auto auto; } .pg > :last-child { grid-column: 1 / -1; } }
    `,
  ];
  /** thumbnail scale (the page is 240 px wide at 1) */
  @property({ type: Number }) scale = 0.5;
  @state() private beforePages: PreviewPage[] | null = null;
  private beforeFor = '';

  protected override willUpdate(): void {
    const s = this.store;
    const key = s.applied?.hash ?? '';
    if (!s.hasDraft || !s.appliedConfig || key === this.beforeFor) return;
    this.beforeFor = key;
    void s.api.preview(s.hub, s.appliedConfig).then((p) => (this.beforePages = p.pages)).catch(() => (this.beforePages = null));
  }

  /** Outlines for the widgets at `idx` of config page `page`, positioned on its decoded wire page. */
  private marks(page: Page | undefined, pp: PreviewPage | undefined | null, idx: number[]): Rect[] {
    const v2 = pp?.page;
    if (!page || !v2 || !idx.length) return [];
    const map = wireMap(page, pp?.wire_pages[0]?.page.widgets.length ?? v2.widgets.length);
    return idx.flatMap((i) => {
      const w = v2.widgets[map[i] ?? -1];
      return w ? [widgetRect(v2, w)] : [];
    });
  }
  private thumb(label: string, pp: PreviewPage | undefined | null, marks: Rect[], theme: RenderTheme) {
    return pp?.page ? html`<figure>${pageSvg(pp.page, { theme }, this.scale, `${label} page`, marks)}<figcaption>${label}</figcaption></figure>` : html`<figure class="none"><span class="muted">${label}: none</span></figure>`;
  }

  protected override render() {
    const s = this.store;
    const applied = s.appliedConfig;
    const pages = s.hasDraft ? changedPages(applied, s.config) : [];
    const before = applied ? activeTheme(applied).theme : null;
    const after = activeTheme(s.config).theme;
    // a theme change recolours every page: show the first few that are not listed already
    const themed = s.hasDraft && !!before && JSON.stringify([before.tokens, before.swatches]) !== JSON.stringify([after.tokens, after.swatches]);
    if (themed)
      for (const pg of (s.config.pages ?? []).slice(0, 3)) if (!pages.some((x) => x.id === pg.id) && (applied?.pages ?? []).some((x) => x.id === pg.id)) pages.push({ id: pg.id, title: pg.title ?? pg.id, kind: 'changed', widgets: [] });
    if (!pages.length) return nothing;
    return html`<p class="legend"><i></i> outlined: widgets that changed${themed ? '. Pages are drawn in the draft theme on the right.' : ''}</p>
      <div class="pages">${pages.map((p) => {
        const ai = (applied?.pages ?? []).findIndex((x) => x.id === p.id);
        const bi = (s.config.pages ?? []).findIndex((x) => x.id === p.id);
        const ch = changedWidgetIndexes(applied?.pages?.[ai] as unknown as Record<string, unknown> | undefined, s.config.pages?.[bi] as unknown as Record<string, unknown> | undefined);
        return html`<div class="pg"><b class="vh">${p.title}</b>
          ${this.thumb('Before', ai >= 0 ? this.beforePages?.[ai] : null, this.marks(applied?.pages?.[ai], ai >= 0 ? this.beforePages?.[ai] : null, p.kind === 'changed' ? ch.before : []), before ?? after)}
          ${this.thumb('After', bi >= 0 ? s.pages[bi] : null, this.marks(s.config.pages?.[bi], bi >= 0 ? s.pages[bi] : null, p.kind === 'changed' ? ch.after : []), after)}
          <div><b>${p.title}</b> <span class="chip ${p.kind === 'added' ? 'ok' : p.kind === 'removed' ? 'bad' : 'ir'}">${KIND_LABEL[p.kind]}</span>${!p.widgets.length && p.kind === 'changed' && themed ? html`<div class="muted">Theme Changed</div>` : nothing}${p.widgets.length ? html`<div class="muted">Widgets: ${p.widgets.slice(0, 6).join(', ')}${p.widgets.length > 6 ? ` and ${p.widgets.length - 6} more` : ''}</div>` : nothing}</div></div>`;
      })}</div>`;
  }
}
