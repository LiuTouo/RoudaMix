// tracks helpers:snapshot table 驅動 meter strip 對應 + 顏色轉換
import type { MeterStrip, TelemetryStripIdentity, Track } from "./types";

function stripFor(
  identity: TelemetryStripIdentity | undefined,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  if (identity === undefined) return undefined;
  const strip = strips?.[identity.id];
  if (strip === undefined || strip.kind !== identity.kind) return undefined;
  const owner = identity.instanceId ?? identity.trackId;
  return owner === null || strip.instanceId === owner ? strip : undefined;
}

export function stripOfTrack(
  trackId: number,
  table: TelemetryStripIdentity[] | undefined,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  return stripFor(
    table?.find((s) => s.trackId === trackId && s.instanceId === null),
    strips,
  );
}

export function stripOfPlugin(
  instanceId: number,
  table: TelemetryStripIdentity[] | undefined,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  return stripFor(
    table?.find((s) => s.instanceId === instanceId),
    strips,
  );
}

export function stripEngineOut(
  table: TelemetryStripIdentity[] | undefined,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  return stripFor(
    table?.find((s) => s.trackId === null && s.instanceId === null),
    strips,
  );
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
