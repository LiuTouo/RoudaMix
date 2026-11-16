/** 使用教學步驟資料（純資料， TutorialWindow 呈現、TutorialDemos 對應動畫）。 */

export type InterfaceVariant = "asio" | "wasapi";

export interface TutorialStep {
  id: string;
  title: string;
  body: string;
  /** TutorialDemos.svelte 的動畫 id。 */
  demo: string;
}

/** 變體前置步驟：音訊裝置設定，ASIO 與 WASAPI 各自的裝置選擇流程。 */
const variantSteps: Record<InterfaceVariant, TutorialStep[]> = {
  asio: [
    {
      id: "asio-device",
      title: "選擇 ASIO 裝置",
      body: "「設定 → 音訊裝置」下拉選單，選你的 ASIO 介面卡。",
      demo: "pick-device",
    },
    {
      id: "asio-buffer",
      title: "調整 Buffer Size",
      body: "越小延遲越低；出現爆音時調大。套用後引擎自動重建。",
      demo: "buffer-size",
    },
    {
      id: "asio-panel",
      title: "ASIO 控制面板",
      body: "取樣率在驅動面板調整；關閉面板後自動重建音訊引擎。",
      demo: "asio-panel",
    },
  ],
  wasapi: [
    {
      id: "wasapi-device",
      title: "選擇系統音訊",
      body: "「設定 → 音訊裝置」選「系統音訊 (WASAPI)」；無 ASIO 硬體也能使用。",
      demo: "pick-wasapi",
    },
    {
      id: "wasapi-note",
      title: "取樣率跟隨系統",
      body: "WASAPI 無 Buffer 選項；取樣率由系統輸出裝置決定。",
      demo: "wasapi-note",
    },
  ],
};

/** 共同步驟：裝置就緒後的新手流程。 */
const commonSteps: TutorialStep[] = [
  {
    id: "add-track",
    title: "新增音軌與輸入來源",
    body: "新增音軌後，來源選麥克風或 Line-in 輸入。",
    demo: "add-track",
  },
  {
    id: "set-output",
    title: "指定輸出目的地",
    body: "軌道送往 Monitor（監聽）、Stream（輸出）或指定聲道。",
    demo: "set-output",
  },
  {
    id: "add-plugin",
    title: "加入 VST 插件",
    body: "點軌道插槽 → 從清單選插件；多個插件依序串接。",
    demo: "add-plugin",
  },
  {
    id: "mix",
    title: "音量、靜音與監聽",
    body: "音量拉桿與電錶在軌條上；靜音鈕切換監聽輸出。",
    demo: "mix",
  },
  {
    id: "session",
    title: "Session 儲存與載入",
    body: "Ctrl+S 儲存場景；「設定 → Session」管理與還原。",
    demo: "session",
  },
];

export function tutorialSteps(variant: InterfaceVariant): TutorialStep[] {
  return [...variantSteps[variant], ...commonSteps];
}
