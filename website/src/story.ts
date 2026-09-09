export const chapters = ['open', 'input', 'plugin', 'route', 'listen', 'obs', 'voice'] as const;
export const motion = { scrollViewports: 8, smoothingSeconds: 0.7, crossfadeSeconds: 0.06 };

export function storyState(progress: number) {
  const p = Math.max(0, Math.min(1, Number.isFinite(progress) ? progress : 0));
  const stage = Math.min(6, Math.floor(p * 7));
  const local = p === 1 ? 1 : p * 7 - stage;
  return { progress: p, stage, local, input: p * 7 >= 1.55, plugin: p * 7 >= 2.54, routed: p * 7 >= 3.62, bypass: p * 7 >= 4.46, obs: p * 7 >= 5.62, voice: p * 7 >= 6.56 };
}

export function chapterProgress(index: number) {
  return (Math.max(0, Math.min(6, index)) + 0.78) / 7;
}
