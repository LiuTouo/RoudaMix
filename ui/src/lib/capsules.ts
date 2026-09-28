import type { MissingPlugin } from "./types.ts";

/** App.svelte 頂層 Notice 的結構子集(結構吻合即可,不搬介面) */
export interface CapsuleNotice {
  id: number;
  kind: "error" | "info";
  msg: string;
  raw?: string;
}

export type CapsuleAction = "dismiss" | "openSettings" | "collapse";

/** 頂端膠囊描述子(純資料;動作由 App.svelte 分派) */
export interface CapsuleItem {
  key: string;
  kind: "error" | "info";
  msg: string;
  tooltip: string;
  /** 點擊膠囊本體的動作 */
  action: CapsuleAction;
  /** action === "dismiss" 時的目標 notice id */
  noticeId?: number;
  /** 有 raw 的通知顯示「複製」鈕 */
  raw?: string;
  /** 右側 × 關閉鈕(狀態驅動警示專用;純通知點本體即關) */
  closable: boolean;
}

export interface CapsuleInputs {
  notices: readonly CapsuleNotice[];
  audioStale: boolean;
  audioStaleDismissed: boolean;
  /** 音訊啟動異常的技術細節(App.svelte 的 notice 字串) */
  audioStaleDetail: string;
  restoreError: string;
  missing: readonly MissingPlugin[];
}

/** 警示在前、通知在後(最新在下);同 App.svelte 既有閱讀順序。 */
export function capsuleItems(input: CapsuleInputs): CapsuleItem[] {
  const items: CapsuleItem[] = [];
  if (input.audioStale && !input.audioStaleDismissed) {
    items.push({
      key: "audioStale",
      kind: "error",
      msg: "音訊未啟動 — 詳情見設定",
      tooltip: input.audioStaleDetail ? `音訊啟動異常：${input.audioStaleDetail}` : "音訊啟動異常:應該在跑但沒跑。",
      action: "openSettings",
      closable: true,
    });
  }
  if (input.restoreError) {
    items.push({
      key: "restoreError",
      kind: "error",
      msg: "Session 恢復失敗,已開空白 — 詳情見設定",
      tooltip: `Session 恢復失敗：${input.restoreError}`,
      action: "openSettings",
      closable: true,
    });
  }
  if (input.missing.length > 0) {
    items.push({
      key: "missing",
      kind: "error",
      msg: `⚠ ${input.missing.length} 個 plugin 無法載入(已保留 placeholder)— 點此收起`,
      tooltip: `未載入的 plugin：\n${input.missing
        .map((m) => `${m.trackName} [${m.index}] ${m.name || m.pluginPath}：${m.message}`)
        .join("\n")}`,
      action: "collapse",
      closable: false,
    });
  }
  for (const n of input.notices) {
    items.push({
      key: `notice-${n.id}`,
      kind: n.kind,
      msg: n.msg,
      tooltip: `詳細訊息：${n.raw ?? n.msg}`,
      action: "dismiss",
      noticeId: n.id,
      raw: n.raw,
      closable: false,
    });
  }
  return items;
}
