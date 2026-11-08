import type { MetersFrame } from "./types";

/** 單一 plugin instance variant 的 EWMA 與峰值(0..1+,顯示端再 ×100) */
export interface PluginLoadStat {
  ewma: number;
  peak: number;
}

/** 與 latencyRuntime 相同的複合鍵:`${instanceId}:${variant}` */
export const pluginLoadKey = (instanceId: number, variant: number) => `${instanceId}:${variant}`;

/**
 * 以一輪 meters pluginLoads 更新常駐的 history 與 view,回傳新 view。
 * 結尾驅逐不在目前圖上的 instance——Drawer 常駐,不驅逐會隨新增/移除
 * plugin 無限成長(LatencyDrawer 先前因此補過同類修復)。
 */
export function observePluginLoads(
  history: Map<string, number[]>,
  previous: Record<string, PluginLoadStat>,
  loads: NonNullable<MetersFrame["pluginLoads"]>,
): Record<string, PluginLoadStat> {
  const next = { ...previous };
  const live = new Set<string>();
  for (const load of loads) {
    const key = pluginLoadKey(load.instanceId, load.variant);
    live.add(key);
    const previousEwma = next[key]?.ewma ?? load.processLoad;
    const ewma = previousEwma + (load.processLoad - previousEwma) / 30;
    const samples = history.get(key) ?? [];
    samples.push(load.processLoad);
    if (samples.length > 150) samples.shift();
    history.set(key, samples);
    next[key] = { ewma, peak: Math.max(...samples) };
  }
  for (const key of history.keys()) if (!live.has(key)) history.delete(key);
  for (const key of Object.keys(next)) if (!live.has(key)) delete next[key];
  return next;
}
