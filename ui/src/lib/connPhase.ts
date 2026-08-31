// P1-A/P1-L:連線狀態的 UI 模型(純邏輯,App.svelte 呼)。
// bridge SharedState.phase 涵蓋 connecting/spawning/connected/spawn_failed/disconnected;
// version mismatch 不在 bridge 知識內(engine reply unsupported_version)→ 由命令
// 錯誤分類補上。標籤/原因對應集中在這,App 只負責顯示。

import type { ConnectionStatus } from "./types";

export type ConnPhase =
  | "connecting"
  | "spawning"
  | "connected"
  | "spawn_failed"
  | "disconnected"
  | "version_mismatch";

export interface ConnView {
  phase: ConnPhase;
  label: string;
  /** 額外原因(spawn 失敗的系統訊息等;空 = 無) */
  detail: string;
  /** ok = 綠燈;warn = 可 retry;err = 需要處理 */
  tone: "ok" | "warn" | "err";
}

const LABELS: Record<ConnPhase, string> = {
  connecting: "連線中…",
  spawning: "引擎啟動中…",
  connected: "已連線",
  spawn_failed: "引擎啟動失敗",
  disconnected: "已斷線 — 引擎仍在背景執行,重試連線中",
  version_mismatch: "引擎協議版本不符",
};

const TONES: Record<ConnPhase, ConnView["tone"]> = {
  connecting: "warn",
  spawning: "warn",
  connected: "ok",
  spawn_failed: "err",
  disconnected: "warn",
  version_mismatch: "err",
};

/** engineCommand 錯誤字串 → 連線分類(unsupported_version = 版本不符) */
export function classifyConnError(err: string): ConnPhase | null {
  if (err.includes("unsupported_version")) return "version_mismatch";
  return null;
}

/** bridge 狀態 → UI 顯示模型。phase 未知(舊 bridge)= connected 判斷 fallback。 */
export function connView(s: ConnectionStatus, probeErr?: string | null): ConnView {
  let phase: ConnPhase;
  if (probeErr) {
    const cls = classifyConnError(probeErr);
    if (cls) return { phase: cls, label: LABELS[cls], detail: probeErr, tone: TONES[cls] };
  }
  const raw = (s.phase ?? (s.connected ? "connected" : "connecting")) as ConnPhase;
  phase = raw in LABELS ? raw : "connecting";
  return { phase, label: LABELS[phase], detail: s.detail ?? "", tone: TONES[phase] };
}

/** P1-A 初始化協調:回傳 listener 註冊後該做的主動同步步驟(純邏輯,測試用)。
 *  規則:永遠 get_snapshot —— 即使 bridge 說已連線(snapshot 事件可能在
 *  listener 掛上前就發過,漏接只能靠主動拉)。 */
export function needsActiveSnapshot(connected: boolean): boolean {
  return true; // 已連線更要拉:事件已錯過
}

/** reconnect epoch 對齊:epoch 變了(新 engine 代)= 上一代的pending 狀態全部作廢。
 *  回傳是否要重置本地 scan/restore 等一次性旗標。 */
export function epochChanged(prev: number, next: number): boolean {
  return prev !== 0 && next !== 0 && prev !== next;
}
