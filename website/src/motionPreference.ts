export type MotionPreference = 'full' | 'reduced';

export function resolveReducedMotion(systemReduced: boolean, preference: string | null) {
  return preference === 'full' ? false : preference === 'reduced' ? true : systemReduced;
}

export function readMotionPreference(): MotionPreference | null {
  const query = new URL(location.href).searchParams.get('motion');
  if (query === 'full' || query === 'reduced') return query;
  try {
    const saved = localStorage.getItem('roudamix-motion-preference');
    return saved === 'full' || saved === 'reduced' ? saved : null;
  } catch { return null; }
}

export function prefersStaticSite() {
  return resolveReducedMotion(matchMedia('(prefers-reduced-motion: reduce)').matches, readMotionPreference());
}

export function chooseMotion(preference: MotionPreference) {
  try {
    localStorage.setItem('roudamix-motion-preference', preference);
    if (preference === 'full') localStorage.setItem('roudamix-demo-mode', 'auto');
  } catch { /* The explicit URL also works when browser storage is unavailable. */ }
  const url = new URL(location.href);
  url.searchParams.set('motion', preference);
  location.assign(url.href);
}
