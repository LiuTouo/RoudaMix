// tracks helpers:snapshot table 驅動 meter strip 對應 + 顏色轉換
import type { MeterStrip, TelemetryStripIdentity, Track } from "./types";

export interface MeterStripView {
  table?: TelemetryStripIdentity[];
  strips?: MeterStrip[];
  // 每 tick 一次建好的 O(1) 索引;缺省時 stripOfTrack 退回線性查找(契約不變)。
  byTrackId?: Map<number, MeterStrip>;
}

function stripFor(
  identity: TelemetryStripIdentity | undefined,
  view: MeterStripView,
): MeterStrip | undefined {
  if (identity === undefined) return undefined;
  const strip = view.strips?.[identity.id];
  if (strip === undefined || strip.kind !== identity.kind) return undefined;
  const owner = identity.instanceId ?? identity.trackId;
  return owner === null || strip.instanceId === owner ? strip : undefined;
}

export function stripOfTrack(
  trackId: number,
  view: MeterStripView,
): MeterStrip | undefined {
  if (view.byTrackId !== undefined) {
    return view.byTrackId.get(trackId);
  }
  return stripFor(
    view.table?.find((s) => s.trackId === trackId && s.instanceId === null),
    view,
  );
}

export function stripOfPlugin(
  instanceId: number,
  view: MeterStripView,
): MeterStrip | undefined {
  return stripFor(
    view.table?.find((s) => s.instanceId === instanceId),
    view,
  );
}

export function stripEngineOut(
  view: MeterStripView,
): MeterStrip | undefined {
  return stripFor(
    view.table?.find((s) => s.trackId === null && s.instanceId === null),
    view,
  );
}

// 每 tick 一次建好的 trackId→strip 索引:語義同 stripOfTrack 的線性查找
// (first-wins + kind/instanceId 校驗),每 tick 由 meterView 建一次,免每 strip 掃表。
export function indexByTrackId(
  view: MeterStripView,
): Map<number, MeterStrip> {
  const m = new Map<number, MeterStrip>();
  if (view.table === undefined || view.strips === undefined) return m;
  for (const identity of view.table) {
    if (identity.instanceId !== null || identity.trackId === null) continue;
    if (m.has(identity.trackId)) continue; // first-wins,同 Array.find
    const strip = view.strips[identity.id];
    if (strip === undefined || strip.kind !== identity.kind) continue;
    if (strip.instanceId !== identity.trackId) continue;
    m.set(identity.trackId, strip);
  }
  return m;
}

// 0xRRGGBB → css "#rrggbb"
export function cssColor(c: number): string {
  return `#${(c & 0xffffff).toString(16).padStart(6, "0")}`;
}

// css "#rrggbb" → 0xRRGGBB(非 syncUI 控件值)
export function parseColor(css: string): number {
  const n = Number.parseInt(css.replace("#", ""), 16);
  return Number.isFinite(n) ? n & 0xffffff : 0;
}

export const inputTrack = (t: Track): boolean => t.kind !== "output";

// #11 路由目的地限縮:來源軌(audio/app)覆寫輸入匯流、路由進去的訊號會被丟棄,
// 「輸出到」清單只列可接收路由的軌道(fx / output),排除自身;順序維持 master 序
export function destCandidates(tracks: Track[], selfId: number): Track[] {
  return tracks.filter(
    (t) => t.trackId !== selfId && t.kind !== "audio" && t.kind !== "app",
  );
}

// 側鏈來源候選:限 input 軌(audio/app;引擎規則鏡像);fx 軌只收 aux input
export function sidechainCandidates(tracks: Track[]): Track[] {
  return tracks.filter((t) => t.kind === "audio" || t.kind === "app");
}
