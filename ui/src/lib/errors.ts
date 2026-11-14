// P1-O:結構化錯誤 {code, message} → 繁中主訊息 + 技術細節保留可複製。
// 映射以 code 直接索引(bridge rejection 已帶型別化 code,無字串解析);
// UI 只顯示 friendly + 展開 raw。

import { toCommandError, type CommandError } from "./protocol-commands.generated.ts";
import type { ErrorCode } from "./error-codes.generated.ts";

export const ENGINE_ERROR_MESSAGES = {
  unsupported_version: "引擎協議版本不符 — 請更新 RoudaMix",
  bad_frame: "通訊框格式錯誤",
  bad_command: "指令參數不合法",
  not_running: "音訊未啟動 — 先按 Start",
  already_running: "音訊已在執行中",
  device_open_failed: "裝置開啟失敗 — 可能被占用或已拔除",
  device_lost: "裝置已失效 — 請重選",
  track_not_found: "找不到軌道（可能已刪除）",
  cycle_detected: "會形成迴圈 — 路由未套用",
  device_busy: "裝置忙碌 — 檢查設定或停用相關軌道",
  app_not_found: "找不到程序 — 可能已關閉",
  unsupported_windows: "Windows 版本過舊（需 Win10 2004+）",
  plugin_not_found: "找不到 plugin（可能已移除）",
  plugin_load_failed: "plugin 載入或初始化失敗",
  plugin_no_editor: "此 plugin 沒有可開啟的操作介面",
  param_not_found: "找不到該參數",
  session_io: "Session 讀寫失敗",
  preset_io: "Preset 讀寫失敗",
  plugin_state_failed: "plugin 狀態同步失敗",
  internal: "內部錯誤",
} satisfies Record<ErrorCode, string>;

const MESSAGES: Record<string, string> = {
  not_connected: "尚未連上引擎 — 自動重試中",
  disconnected: "連線中斷 — 指令未送達",
  timeout: "引擎無回應（逾時）",
  ...ENGINE_ERROR_MESSAGES,
};

export interface FriendlyError {
  /** 繁中主訊息(未知的 code = 原 message) */
  friendly: string;
  /** code + 原 message(可複製、可展開的技術細節) */
  raw: string;
}

/** 錯誤物件(或任意 rejected value)→ "code: message" 顯示字串 */
export function errorText(err: unknown): string {
  const e = toCommandError(err);
  return `${e.code}: ${e.message}`;
}

/** 結構化錯誤 → 顯示模型 */
export function friendlyError(err: unknown): FriendlyError {
  const e = toCommandError(err);
  const raw = errorText(e);
  const known = MESSAGES[e.code];
  return { friendly: known ?? (e.message || raw), raw };
}

/** P1-J/P1-O:負載/過載警示 debounce —— 連續 N 次超標才算持續過載
 *  (單次尖峰不洗版),低於門檰連續 N 次才解除。 */
export class OverloadDetector {
  private over = 0;
  private under = 0;
  private threshold: number;
  private hitCount: number;
  private clearCount: number;

  constructor(threshold: number, hitCount: number, clearCount: number) {
    this.threshold = threshold;
    this.hitCount = hitCount;
    this.clearCount = clearCount;
  }

  /** 餵新樣本;回傳「警示狀態是否改變」(只在轉換時通知) */
  sample(v: number): boolean {
    if (v > this.threshold) {
      this.under = 0;
      this.over++;
      if (this.over === this.hitCount) return true; // 剛轉為過載
    } else {
      this.over = 0;
      this.under++;
      if (this.under === this.clearCount) return true; // 剛解除
    }
    return false;
  }

  get isOverloaded(): boolean {
    return this.over >= this.hitCount;
  }
}
