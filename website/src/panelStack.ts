/** Pin a tall panel only after its bottom has been read, then let the next cover it. */
export function installPanelStack(panels: HTMLElement[]) {
  const measure = () => {
    for (const panel of panels) panel.style.setProperty('--stack-top', `${Math.min(0, innerHeight - panel.offsetHeight)}px`);
  };
  const observer = new ResizeObserver(measure);
  panels.forEach(panel => observer.observe(panel));
  window.addEventListener('resize', measure);
  measure();
  return () => {
    observer.disconnect();
    window.removeEventListener('resize', measure);
    panels.forEach(panel => panel.style.removeProperty('--stack-top'));
  };
}
