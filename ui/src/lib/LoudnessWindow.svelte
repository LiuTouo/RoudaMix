<script lang="ts">
  // 響度分析獨立視窗(main.ts 以 ?window=loudness 掛載本組件)。
  // 資料不落地:主視窗 LoudnessController 仍是唯一擁有者(engine IPC、生命週期),
  // 經 Tauri event 推 view 過來;重置/關閉以 event 回投主視窗。獨立 OS 視窗 =
  // 釘選(本視窗自身 always-on-top)可跨程式置頂而不連帶主視窗 —— 這是舊
  // in-window popover 做不到的;未釘選時 owner 關係讓它僅浮在 RoudaMix 之上。
  import { onMount } from "svelte";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import { emit, listen } from "@tauri-apps/api/event";
  import { emptyLoudnessView, loudnessDuration, loudnessValue, type LoudnessView } from "./loudness";

  interface PanelPayload {
    view: LoudnessView;
    trackName: string;
    trackKind: string;
    running: boolean;
  }
  let panel = $state<PanelPayload>({
    view: emptyLoudnessView(), trackName: "", trackKind: "input", running: false,
  });
  let pinned = $state(false);
  const report = $derived(panel.view.report);
  const interrupted = $derived(!!panel.view.error || report?.state === "error");
  const statusText = $derived(panel.view.busy ? "開始量測…" : interrupted ? "量測中斷" :
    !panel.running ? (report ? "已停止 · 數值凍結" : "引擎未啟動") :
    report?.state === "stopped" ? "已停止 · 數值凍結" :
    !report || report.elapsedSeconds < 3 ? "收集資料中…" : "量測中");
  const metrics = [
    { key: "momentaryLufs", label: "LUFS-M", window: "瞬時 · 400 ms", help: "最近 400 毫秒的響度。" },
    { key: "shortTermLufs", label: "LUFS-S", window: "短期 · 3 秒", help: "最近 3 秒的響度。" },
    { key: "integratedLufs", label: "LUFS-I", window: "整體 · 本次", help: "本次量測的整體響度，排除過低音量。" },
  ] as const;

  // 釘選 = 本視窗跨程式永遠置頂(與 VST 編輯器視窗釘選同語意);淡紫色 = 啟用
  async function togglePin() {
    try {
      await getCurrentWindow().setAlwaysOnTop(!pinned);
    } catch {
      // 非 Tauri 環境(測試/原型)無視窗 API:僅更新樣式不噴錯
    }
    pinned = !pinned;
  }
  function closePanel() {
    void getCurrentWindow().close().catch(() => {});
  }
  function resetPanel() {
    void emit("loudness-panel:reset");
  }
  // 整窗可拖：空白處 pointerdown 即拖視窗（鈕除外）——面板是唯讀顯示，無文字
  // 選取需求，preventDefault 播掉選取/捲動預設。startDragging = 原生視窗拖曳。
  function startWindowDrag(event: PointerEvent) {
    if (event.button !== 0) return;
    if (event.target instanceof Element && event.target.closest("button")) return;
    event.preventDefault();
    void getCurrentWindow().startDragging().catch(() => {});
  }

  onMount(() => {
    // ready = 面板掛好,主視窗補推現況(開窗即有內容,不等下一次量測輪詢)
    void emit("loudness-panel:ready");
    const unlisten = listen<PanelPayload>("loudness-panel:view", (e) => (panel = e.payload));
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== "Escape" || event.defaultPrevented) return;
      event.preventDefault();
      closePanel();
    };
    window.addEventListener("keydown", onKey);
    return () => {
      void unlisten.then((u) => u());
      window.removeEventListener("keydown", onKey);
    };
  });
</script>

<!-- 標題列無特別處理：拖曳由根容器 startWindowDrag 統一處理（整窗可拖） -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="loudness-window" onpointerdown={startWindowDrag}>
  <header>
    <button type="button" class="pin-toggle" class:pinned aria-pressed={pinned}
      aria-label={pinned ? "取消視窗置頂" : "釘選視窗置頂"}
      data-tooltip={pinned ? "取消置頂：面板回到僅在 RoudaMix 內浮上層。"
        : "釘選：此面板永遠保持在螢幕最上層（RoudaMix 主視窗不會一起置頂）。"}
      onclick={togglePin}>
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path d="M12 17v5" /><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V6h1a2 2 0 0 0 0-4H8a2 2 0 0 0 0 4h1z" />
      </svg>
    </button>
    <h2 id="loudness-heading">響度分析 <span>· {panel.trackName}</span></h2>
    <button type="button" class="dialog-close" aria-label="關閉響度分析" data-tooltip="關閉並停止量測。"
      onclick={closePanel}>×</button>
  </header>
  <p class="status" role="status">{statusText}</p>
  <div class="loudness-values">
    {#each metrics as metric}
      <div class="metric" data-tooltip={metric.help}>
        <span class="metric-label">{metric.label}</span>
        <strong class="metric-value" data-metric={metric.key}>{loudnessValue(report, metric.key)}</strong>
        <span class="metric-window">{metric.window}</span>
      </div>
    {/each}
  </div>
  <dl>
    <div><dt data-tooltip="本次最高真峰值，包含取樣點之間的峰值。">最高 True Peak</dt>
      <dd data-metric="truePeakDbtp">{loudnessValue(report, "truePeakDbtp")} <span>dBTP</span></dd></div>
    <div><dt data-tooltip="響度的變化範圍；越小越平穩，越大起伏越明顯。">響度範圍（LRA）</dt>
      <dd data-metric="loudnessRangeLu">{loudnessValue(report, "loudnessRangeLu")} <span>LU</span></dd></div>
  </dl>
  <!-- 成品參考：https://podcasters.apple.com/support/893-audio-requirements
       https://support.spotify.com/us/artists/article/loudness-normalization/ -->
  <section class="reference" aria-labelledby="loudness-reference">
    <h3 id="loudness-reference">成品輸出參考 <span>LUFS-I</span></h3>
    <div class="reference-targets">
      <div><span>Apple Podcasts</span><strong>−16 ±1 LUFS</strong></div>
      <div><span>Spotify 音樂</span><strong>−14 LUFS</strong></div>
    </div>
    <p class="reference-peak">True Peak <strong>≤ −1 dBTP</strong></p>
    <p class="note">Spotify 高於 −14 LUFS 時，真峰值建議低於 −2 dBTP。</p>
    <p class="note">非單軌目標；M／S、LRA 不設固定值。</p>
  </section>
  {#if interrupted}<p class="error" role="alert">{panel.view.error || report?.error || "量測中斷，請重置後再試。"}</p>{/if}
  {#if panel.view.restarted && !panel.view.busy}<p class="note">引擎變更，量測已重置。</p>{/if}
  <footer>
    <span>已量測 <time>{loudnessDuration(report?.elapsedSeconds ?? 0)}</time></span>
    <button type="button" disabled={!panel.running || panel.view.busy} onclick={resetPanel}
      aria-label="重置響度量測" data-tooltip="清空數值與計時，重新量測。">{!report && panel.view.error ? "重試量測" : "重置量測"}</button>
  </footer>
  <p class="note">— 無有效資料 · −∞ 靜音 · 開啟或重置後累積</p>
  <p class="note">{panel.trackKind === "output" ? "量測點：實際輸出（含監聽）。" : "量測點：插件、音量與靜音控制之後。"}</p>
</div>

<style>
  /* 樣式承襲舊 LoudnessPopover(app.css 浮層語彙):console-surface 底 + 1px hairline
     + 3px 圓角 + 無投影;視窗外框由 Tauri(decorations:false + shadow)提供。 */
  .loudness-window {
    box-sizing: border-box;
    height: 100vh;
    overflow: auto;
    padding: 16px;
    background: var(--surface);
    color: var(--text);
    font: 12px/1.45 var(--font-ui);
  }
  header { display: flex; align-items: center; justify-content: space-between; gap: 12px;
    margin: 0 -16px; padding: 0 16px 10px; border-bottom: 1px solid var(--border);
    cursor: grab; user-select: none; }
  /* 釘選鈕:與 .dialog-close 同款幽靈圖示鈕;釘選中 = 淡紫(與 VST 編輯器視窗
     釘選色 #C4B5FD 同值,兩視窗統一圖示與色彩語彙) */
  .pin-toggle { display: grid; flex: 0 0 auto; place-items: center; width: 28px; height: 28px;
    padding: 0; background: none; border: none; border-radius: var(--flat-radius);
    color: var(--text-dim); }
  .pin-toggle:hover { background: var(--bg-raised); color: var(--text); }
  .pin-toggle.pinned, .pin-toggle.pinned:hover { color: #c4b5fd; }
  .pin-toggle svg { width: 16px; height: 16px; fill: none; stroke: currentColor;
    stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; }
  /* 標題 = DESIGN Title:600/14px/+0.3px(全系統最大字級) */
  h2 { margin: 0; font: 600 14px/1.45 var(--font-ui); letter-spacing: .3px; min-width: 0;
    flex: 1 1 auto; overflow-wrap: anywhere; }
  h2 span { font-weight: 400; letter-spacing: 0; }
  .status { margin: 10px 0 12px; color: var(--text-dim); }
  /* 三讀值欄以暗分割線分段(DESIGN:同群組次級分割),數值 mono + tabular-nums */
  .loudness-values { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); padding: 4px 0 16px; }
  .metric { display: flex; flex-direction: column; align-items: center; gap: 2px; padding: 2px 8px; }
  .metric + .metric { border-left: 1px solid var(--divider); }
  .metric-label, .metric-window { color: var(--text-dim); font-size: 11px; }
  .metric-value { font: 600 14px/1.4 var(--mono); font-variant-numeric: tabular-nums; }
  dl { border-top: 1px solid var(--border); margin: 0; padding: 10px 0; }
  dl > div { display: flex; justify-content: space-between; gap: 12px; padding: 5px 0; }
  dt { color: var(--text-dim); font-size: 11px; }
  dd { margin: 0; white-space: nowrap; font: 600 12px var(--mono); font-variant-numeric: tabular-nums; }
  dd span { font: 400 11px var(--font-ui); color: var(--text-dim); }
  .reference { border-top: 1px solid var(--border); padding: 10px 0 12px; }
  .reference h3 { display: flex; align-items: baseline; gap: 8px; margin: 0 0 8px;
    font-size: 11px; font-weight: 500; letter-spacing: .04em; }
  .reference h3 span { font: 400 11px var(--mono); color: var(--text-dim); letter-spacing: 0; }
  .reference-targets span { color: var(--text-dim); font-size: 11px; font-weight: 400; }
  .reference-targets { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
  .reference-targets > div { display: flex; flex-direction: column; gap: 2px; }
  .reference strong { font: 500 12px var(--mono); font-variant-numeric: tabular-nums; }
  .reference-peak { margin: 8px 0 0; font-size: 11px; }
  .reference-peak strong { font-weight: 500; }
  .reference .note { margin-top: 4px; }
  footer { border-top: 1px solid var(--border); padding-top: 10px; color: var(--text-dim); font-size: 11px; }
  footer button { min-height: 28px; }
  time { font-variant-numeric: tabular-nums; }
  .note { margin: 10px 0 0; color: var(--text-dim); font-size: 11px; }
  .error { color: var(--err); overflow-wrap: anywhere; }
</style>
