<script lang="ts">
  import { onMount } from "svelte";
  import { loudnessDuration, loudnessValue, type LoudnessView } from "./loudness";
  import type { Track } from "./types";

  let { track, view, running, onClose, onReset }: {
    track: Track;
    view: LoudnessView;
    running: boolean;
    onClose: (restoreFocus?: boolean) => void;
    onReset: () => void;
  } = $props();
  let panel: HTMLDivElement;
  let header: HTMLElement;
  let dragging = $state(false);
  const report = $derived(view.report);
  const interrupted = $derived(!!view.error || report?.state === "error");
  const statusText = $derived(view.busy ? "開始量測…" : interrupted ? "量測中斷" :
    !running ? (report ? "已停止 · 數值凍結" : "引擎未啟動") :
    report?.state === "stopped" ? "已停止 · 數值凍結" :
    !report || report.elapsedSeconds < 3 ? "收集資料中…" : "量測中");
  const metrics = [
    { key: "momentaryLufs", label: "LUFS-M", window: "瞬時 · 400 ms", help: "最近 400 毫秒的響度。" },
    { key: "shortTermLufs", label: "LUFS-S", window: "短期 · 3 秒", help: "最近 3 秒的響度。" },
    { key: "integratedLufs", label: "LUFS-I", window: "整體 · 本次", help: "本次量測的整體響度，排除過低音量。" },
  ] as const;
  const EDGE = 8;

  // 浮動面板(對齊 VST 編輯器視窗行為):開啟後固定位置,不跟隨錨點、不因捲動
  // 自動關閉;與主畫面其他內容無 z-order 競爭(popover top layer),置頂僅存在
  // 於 RoudaMix 視窗內,切換到別的程式自然被蓋過去。
  function clampToViewport(x: number, y: number) {
    const rect = panel.getBoundingClientRect();
    const maxX = Math.max(EDGE, window.innerWidth - rect.width - EDGE);
    const maxY = Math.max(EDGE, window.innerHeight - rect.height - EDGE);
    panel.style.left = `${Math.min(Math.max(EDGE, x), maxX)}px`;
    panel.style.top = `${Math.min(Math.max(EDGE, y), maxY)}px`;
  }

  // 預設落在主介面「假想中線」左側(中間靠左):右緣距中線 12px,垂直置中。
  // 與 VST 編輯器視窗的預設位(中線右側)對稱,同時開啟時彼此不重疊。
  function placeDefault() {
    const rect = panel.getBoundingClientRect();
    const center = window.innerWidth / 2;
    const x = Math.max(EDGE, center - rect.width - 12);
    const y = Math.max(EDGE, (window.innerHeight - rect.height) / 2);
    panel.style.left = `${x}px`;
    panel.style.top = `${y}px`;
  }

  // 標題列 = 拖曳把手(關閉鈕除外);move/up 掛 window,游標衝出把手也跟得住。
  function startDrag(event: PointerEvent) {
    if (event.button !== 0) return;
    if (event.target instanceof Element && event.target.closest("button")) return;
    const rect = panel.getBoundingClientRect();
    const offsetX = event.clientX - rect.left;
    const offsetY = event.clientY - rect.top;
    dragging = true;
    event.preventDefault();
    const move = (e: PointerEvent) => clampToViewport(e.clientX - offsetX, e.clientY - offsetY);
    const up = () => {
      dragging = false;
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  }

  $effect(() => {
    panel?.focus({ preventScroll: true });
  });

  onMount(() => {
    panel.showPopover();
    panel.focus({ preventScroll: true });
    placeDefault();
    // 鍵盤搬移:面板聚焦(panel 開啟即聚焦)時用方向鍵微調位置 = 拖曳的鍵盤替代。
    const onPanelKey = (event: KeyboardEvent) => {
      const deltas: Record<string, [number, number]> = {
        ArrowLeft: [-16, 0], ArrowRight: [16, 0], ArrowUp: [0, -16], ArrowDown: [0, 16],
      };
      const delta = deltas[event.key];
      if (!delta || event.altKey || event.ctrlKey || event.metaKey) return;
      const rect = panel.getBoundingClientRect();
      clampToViewport(rect.left + delta[0], rect.top + delta[1]);
      event.preventDefault();
    };
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== "Escape" || event.defaultPrevented || document.querySelector("dialog[open]")) return;
      if (event.target instanceof Element) {
        const otherPopover = event.target.closest("[popover]");
        if (otherPopover && otherPopover !== panel) return;
      }
      event.preventDefault();
      event.stopPropagation();
      onClose(true);
    };
    window.addEventListener("keydown", onKey);
    panel.addEventListener("keydown", onPanelKey);
    return () => {
      window.removeEventListener("keydown", onKey);
      panel.removeEventListener("keydown", onPanelKey);
      panel.hidePopover();
    };
  });
</script>

<div bind:this={panel} id="loudness-analysis" class="loudness-popup" popover="manual"
  role="dialog" tabindex="-1" aria-labelledby="loudness-heading">
  <!-- 拖曳把手:指標拖曳;鍵盤以 panel 焦點 + 方向鍵搬移(onPanelKey) -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <header bind:this={header} class:dragging onpointerdown={startDrag}>
    <h2 id="loudness-heading">響度分析 <span>· {track.name}</span></h2>
    <button type="button" class="dialog-close" aria-label="關閉響度分析" data-tooltip="關閉並停止量測。"
      onclick={() => onClose(true)}>×</button>
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
  {#if interrupted}<p class="error" role="alert">{view.error || report?.error || "量測中斷，請重置後再試。"}</p>{/if}
  {#if view.restarted && !view.busy}<p class="note">引擎變更，量測已重置。</p>{/if}
  <footer>
    <span>已量測 <time>{loudnessDuration(report?.elapsedSeconds ?? 0)}</time></span>
    <button type="button" disabled={!running || view.busy} onclick={onReset}
      aria-label="重置響度量測" data-tooltip="清空數值與計時，重新量測。">{!report && view.error ? "重試量測" : "重置量測"}</button>
  </footer>
  <p class="note">— 無有效資料 · −∞ 靜音 · 開啟或重置後累積</p>
  <p class="note">{track.kind === "output" ? "量測點：實際輸出（含監聽）。" : "量測點：插件、音量與靜音控制之後。"}</p>
</div>

<style>
  /* 浮動面板掛在 .audio-console 之外,自帶浮層語彙(DESIGN.md 浮層):
     console-surface 底 + 1px hairline + 3px 圓角 + 無投影;token 由
     flat-theme.css 浮層規則(.loudness-popup 列入浮層同名單)供給。 */
  .loudness-popup {
    position: fixed; inset: auto; margin: 0; padding: 16px; box-sizing: border-box;
    width: min(440px, calc(100vw - 16px));
    max-height: calc(100vh - 16px);
    overflow: auto;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--flat-radius);
    box-shadow: none;
    color: var(--text);
    font: 12px/1.45 var(--font-ui);
  }
  .loudness-popup::backdrop { background: transparent; pointer-events: none; }
  header, footer { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  header { cursor: grab; user-select: none; touch-action: none; margin: 0 -16px; padding: 0 16px 10px;
    border-bottom: 1px solid var(--border); }
  header.dragging { cursor: grabbing; }
  /* 標題 = DESIGN Title:600/14px/+0.3px(全系統最大字級) */
  h2 { margin: 0; font: 600 14px/1.45 var(--font-ui); letter-spacing: .3px; min-width: 0; overflow-wrap: anywhere; }
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
