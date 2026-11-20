/** Spans from measured fit, not character counts: a chip whose label is cut takes two cells, and the whole row if two are still too few. */
export function fitCells(root: ParentNode): void {
  for (const grid of root.querySelectorAll<HTMLElement>('.cells')) {
    const kids = [...grid.children].filter((k): k is HTMLElement => k instanceof HTMLElement && k.tagName === 'BUTTON');
    if (!kids.length) continue;
    kids.forEach((k) => k.removeAttribute('data-span'));
    const cols = getComputedStyle(grid).gridTemplateColumns.split(' ').filter(Boolean).length;
    const cut = (k: HTMLElement) => k.scrollWidth > k.clientWidth;
    const wide = kids.filter(cut);
    wide.forEach((k) => k.setAttribute('data-span', cols > 1 ? '2' : 'full'));
    wide.filter(cut).forEach((k) => k.setAttribute('data-span', 'full'));
  }
}

/** Re-fit now, when the box resizes and when web fonts land. Returns a stop function. */
export function watchCells(el: HTMLElement, root: ParentNode): () => void {
  let raf = 0;
  const run = () => {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(() => fitCells(root));
  };
  const ro = new ResizeObserver(run);
  ro.observe(el);
  void document.fonts?.ready.then(run);
  return () => {
    ro.disconnect();
    cancelAnimationFrame(raf);
  };
}
