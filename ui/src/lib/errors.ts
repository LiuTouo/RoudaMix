// P1-O:raw backend error("code: message")→ 繁中主訊息 + 技術細節保留可複製。
// 映射集中在這;UI 只顯示 friendly + 展開 raw。

import type { ErrorCode } from "./error-codes.generated.ts";

export const ENGINE_ERROR_MESSAGES = {
  unsupported_version: "引擎協議版本不符 — 請更新 RoudaMix(bridge 與 engine 需同版)",
  bad_frame: "通訊框格式錯誤",
  bad_command: "指令參數不合法",
  not_running: "音訊未啟動 — 先按 Start",
  already_running: "音訊已在執行中",
  device_open_failed: "音訊裝置開啟失敗 — 裝置可能被其他程式占用,或已拔除",
  device_lost: "音訊裝置已失效 — 請重新選擇裝置",
  track_not_found: "找不到該軌道(可能已被刪除)",
  cycle_detected: "這樣接會形成迴圈 — 路由未套用",
  device_busy: "裝置/聲道已被其他軌道使用 — 換一組或先停用原軌",
  app_not_found: "找不到該程序 — 程式可能已關閉,請重新選擇",
  unsupported_windows: "此 Windows 版本不支援抓取應用程式音訊(process loopback 需 Win10 2004+)",
  plugin_not_found: "找不到該 plugin(可能已被移除)",
  plugin_load_failed: "plugin 載入或初始化失敗 — 請查看技術詳細資訊；必要時可重新定位",
  plugin_no_editor: "此 plugin 沒有可開啟的操作介面",
  param_not_found: "找不到該參數",
  session_io: "Session 檔讀寫失敗 — 路徑可能無法存取,或檔案損壞",
  preset_io: "Preset 檔讀寫失敗 — 檔案可能不存在或格式不符",
  plugin_state_failed: "plugin 狀態同步失敗 — 原設定與目前音訊路徑已保留",
  internal: "內部錯誤",
} satisfies Record<ErrorCode, string>;

const MESSAGES: Record<string, string> = {
  not_connected: "尚未連上引擎 — 稍後自動重試,或按「重試連線」",
  disconnected: "與引擎的連線中斷 — 指令未送達",
  timeout: "引擎沒有回應(逾時)— 引擎可能忙碌或已停止",
  ...ENGINE_ERROR_MESSAGES,
};

export interface FriendlyError {
  /** 繁中主訊息(未知的 code = 原字串) */
  friendly: string;
  /** 原始字串(可複製、可展開的技術細節) */
  raw: string;
}

/** "code: message" 或任意錯誤字串 → 顯示模型 */
export function friendlyError(err: string): FriendlyError {
  const m = /^([a-z_]+):\s*(.*)$/i.exec(err);
  if (m && MESSAGES[m[1]]) {
    return { friendly: MESSAGES[m[1]], raw: m[2] ? `${m[1]}: ${m[2]}` : m[1] };
  }
  return { friendly: err, raw: err };
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
