import type { Track } from "./types";

export interface LatencyRuntimeNotice {
  level: "error" | "info";
  text: string;
}

/**
 * 比對 plugin 執行狀態變化並維護觀察 Map（key = `${instanceId}:${variant}`）。
 * 狀態有變才產生通知；結尾驅逐不在目前圖上的 instance——Map 常駐，
 * 不驅逐會隨新增/移除 plugin 無限成長，且復用 instanceId 時 stale 狀態
 * 會吞掉應有的降級警示或誤發「已恢復」通知（同 LatencyDrawer 負載歷史）。
 */
export function observeLatencyRuntimeStates(
  states: Map<string, string>,
  tracks: Track[],
): LatencyRuntimeNotice[] {
  const notices: LatencyRuntimeNotice[] = [];
  const live = new Set<string>();
  for (const track of tracks) {
    for (const plugin of track.plugins) {
      for (const [variant, state] of [
        ["primary", plugin.runtimeState ?? "active"],
        ["monitor", plugin.monitorState ?? "active"],
      ] as const) {
        const key = `${plugin.instanceId}:${variant}`;
        live.add(key);
        const previous = states.get(key);
        states.set(key, state);
        if (state === previous) continue;
        const role = variant === "monitor" ? "Monitor Shadow" : "Primary";
        if (state === "suspended" || state === "degraded")
          notices.push({
            level: "error",
            text: `${plugin.name} 的 ${role} 已${state === "suspended" ? "暫停" : "降級"}，改送 dry audio。`,
          });
        else if (state === "active" && previous && previous !== "active")
          notices.push({ level: "info", text: `${plugin.name} 的 ${role} 已恢復。` });
      }
    }
  }
  for (const key of states.keys()) if (!live.has(key)) states.delete(key);
  return notices;
}
