import { LitElement } from 'lit';
import { getStore, type Store } from '../lib/store';

/** Element that re-renders whenever the draft store changes. */
export class StoreElement extends LitElement {
  protected store: Store = getStore();
  private onChange = () => this.requestUpdate();
  override connectedCallback(): void {
    super.connectedCallback();
    this.store.addEventListener('change', this.onChange);
  }
  override disconnectedCallback(): void {
    this.store.removeEventListener('change', this.onChange);
    super.disconnectedCallback();
  }
}

export interface ConfirmRequest {
  title: string;
  body: string;
  ok: string;
  danger?: boolean;
  resolve: (yes: boolean) => void;
}
/** Ask the app-level confirm dialog; resolves false when nothing is listening. */
export function confirmAction(from: HTMLElement, req: Omit<ConfirmRequest, 'resolve'>): Promise<boolean> {
  return new Promise((resolve) => {
    const handled = !from.dispatchEvent(
      new CustomEvent<ConfirmRequest>('hh-confirm', { detail: { ...req, resolve }, bubbles: true, composed: true, cancelable: true }),
    );
    if (!handled) resolve(false);
  });
}
