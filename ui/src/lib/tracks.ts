// tracks helpers:meter strip 查表(ABI v3:id+kind,不靠順序)+ 顏色轉換
import type { MeterStrip, Track } from "./types";

export const STRIP_TRACK = 1; // MeterStrip.kind:trackId
export const STRIP_ENGINE_OUT = 2;

// 軌 meter:以 (trackId, kind=1) 查 SHM strip;查不到 = 該幀沒資料(undefined = 靜音錶)
export function stripOfTrack(
  trackId: number,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  return strips?.find((s) => s.kind === STRIP_TRACK && s.instanceId === trackId);
}

export function stripOfPlugin(
  instanceId: number,
  strips: MeterStrip[] | undefined,
): MeterStrip | undefined {
  return strips?.find((s) => s.kind === 0 && s.instanceId === instanceId);
}

export function stripEngineOut(strips: MeterStrip[] | undefined): MeterStrip | undefined {
  return strips?.find((s) => s.kind === STRIP_ENGINE_OUT);
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
