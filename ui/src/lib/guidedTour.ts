/** 引導式教學步驟資料（GuidedTour 呈現）。
 *  selector 對應主視窗真實控制項的 data-tour 錨點；"{track}" 由 GuidedTour
 *  以實際示範音軌 id 代換。landing（選介面）與完成頁由 GuidedTour 自身狀態呈現。 */

export type InterfaceVariant = "asio" | "wasapi";

export interface TourStep {
  id: string;
  /** 目標 CSS selector；"{track}" 代換為示範音軌 id。 */
  selector: string;
  title: string;
  body: string;
  /** read = 按「下一步」前進；click = 點擊高亮目標前進（下一步可代行）。 */
  mode: "read" | "click";
  /** 進入此步前自動開啟設定並切到分頁。 */
  opensSettings?: "audio" | "scenes";
  /** 離開此步時自動關閉設定。 */
  closesSettings?: boolean;
}

/** 變體前置步驟：設定 → 音訊裝置分頁內的裝置選擇流程。 */
const variantSteps: Record<InterfaceVariant, TourStep[]> = {
  asio: [
    {
      id: "open-settings",
      selector: '[data-tour="settings-gear"]',
      title: "開啟設定",
      body: "點右下角的設定（齒輪）鈕；或按「下一步」由教學代為開啟。",
      mode: "click",
    },
    {
      id: "asio-device",
      selector: '[data-tour="set-device"]',
      title: "選擇 ASIO 裝置",
      body: "在「音訊裝置」下拉選單選你的 ASIO 介面卡；切換後引擎自動重建。",
      mode: "read",
      opensSettings: "audio",
    },
    {
      id: "asio-buffer",
      selector: '[data-tour="set-buffer"]',
      title: "調整 Buffer Size",
      body: "越小延遲越低；出現爆音時調大。",
      mode: "read",
      opensSettings: "audio",
    },
    {
      id: "asio-panel",
      selector: '[data-tour="set-asiopanel"]',
      title: "ASIO 控制面板",
      body: "取樣率在驅動面板調整；關閉面板後自動重建音訊引擎。",
      mode: "read",
      opensSettings: "audio",
      closesSettings: true,
    },
  ],
  wasapi: [
    {
      id: "open-settings",
      selector: '[data-tour="settings-gear"]',
      title: "開啟設定",
      body: "點右下角的設定（齒輪）鈕；或按「下一步」由教學代為開啟。",
      mode: "click",
    },
    {
      id: "wasapi-device",
      selector: '[data-tour="set-device"]',
      title: "選擇系統音訊",
      body: "「音訊裝置」選「系統音訊 (WASAPI)」；無 ASIO 硬體也能使用。",
      mode: "read",
      opensSettings: "audio",
    },
    {
      id: "wasapi-note",
      selector: '[data-tour="set-device"]',
      title: "取樣率跟隨系統",
      body: "WASAPI 無 Buffer 選項；取樣率由系統輸出裝置決定。",
      mode: "read",
      opensSettings: "audio",
      closesSettings: true,
    },
  ],
};

/** 共同步驟：裝置就緒後，從新增音軌到輸出目的地、插件與電錶的實體引導。 */
const commonSteps: TourStep[] = [
  {
    id: "add-track",
    selector: '[data-tour="add-audio"]',
    title: "新增音軌",
    body: "點「＋ Audio」新增一條音軌，教學會沿這條軌繼續；或按「下一步」代為新增。已有軌時教學直接沿用現有軌。",
    mode: "click",
  },
  {
    id: "track-source",
    selector: '[data-track-id="{track}"] [data-tour="source"]',
    title: "選擇輸入來源",
    body: "「來源」設定這軌的輸入：ASIO 聲道、系統麥克風，或 App 軌的應用程式。",
    mode: "read",
  },
  {
    id: "track-dest",
    selector: '[data-track-id="{track}"] [data-tour="dest"]',
    title: "指定輸出目的地",
    body: "「送至」勾選 Monitor（監聽）、Stream（輸出）或指定聲道。",
    mode: "read",
  },
  {
    id: "track-plugin",
    selector: '[data-track-id="{track}"] [data-tour="plugin-add"]',
    title: "加入 VST 插件",
    body: "點 VST 面板的「＋」從清單選插件；多個插件依序串接，點擊已裝插件可開編輯器。",
    mode: "read",
  },
  {
    id: "track-fader",
    selector: '[data-track-id="{track}"] [data-tour="fader"]',
    title: "音量與靜音",
    body: "拉桿調音量（滾輪微調、Ctrl+點擊回 100%）；錶下方「M」靜音此軌。",
    mode: "read",
  },
  {
    id: "track-meter",
    selector: '[data-track-id="{track}"] [data-tour="meter"]',
    title: "電錶",
    body: "電錶即時顯示這軌的音量，過載會標記。",
    mode: "read",
  },
  {
    id: "app-track",
    selector: '[data-tour="add-app"]',
    title: "App 軌：捕捉應用程式",
    body: "「＋ App」新增應用程式軌，選擇正在出聲的程式（如瀏覽器、Discord）只捕捉它的聲音；可隨時更換或解除。",
    mode: "read",
  },
  {
    id: "fx-track",
    selector: '[data-tour="add-fx"]',
    title: "FX 軌：全域效果",
    body: "「＋ FX」新增效果軌：本身不擷取輸入，讓其他軌「送至」它，插件處理後再送往輸出（如全域壓縮、等化）。",
    mode: "read",
  },
  {
    id: "session",
    selector: '[data-tour="set-sessions"]',
    title: "Session 儲存與載入",
    body: "Ctrl+S 隨時儲存場景；設定 → Session 可管理與還原。",
    mode: "read",
    opensSettings: "scenes",
    closesSettings: true,
  },
];

export function guidedTourSteps(variant: InterfaceVariant): TourStep[] {
  return [...variantSteps[variant], ...commonSteps];
}
