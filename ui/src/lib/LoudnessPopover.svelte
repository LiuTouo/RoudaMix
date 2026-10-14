<script lang="ts">
  import { onMount } from "svelte";
  import { loudnessDuration, loudnessValue, type LoudnessView } from "./loudness";
  import type { Track } from "./types";

  let { track, anchor, view, running, onClose, onReset }: {
    track: Track;
    anchor: HTMLElement;
    view: LoudnessView;
    running: boolean;
    onClose: (restoreFocus?: boolean) => void;
    onReset: () => void;
  } = $props();
  let panel: HTMLDivElement;
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

  $effect(() => {
    const currentAnchor = anchor;
    panel?.focus({ preventScroll: true });
    const observer = typeof IntersectionObserver === "undefined" ? null : new IntersectionObserver((entries) => {
      if (entries.some((entry) => !entry.isIntersecting)) onClose(false);
    });
    observer?.observe(currentAnchor);
    return () => observer?.disconnect();
  });

  onMount(() => {
    panel.showPopover();
    panel.focus({ preventScroll: true });
    let raf = 0;
    let lastPosition = "";
    const position = () => {
      if (!anchor.isConnected) { onClose(false); return; }
      const rect = anchor.getBoundingClientRect();
      const edge = 8;
      const width = Math.min(440, Math.max(0, window.innerWidth - edge * 2));
      const maxHeight = Math.max(0, window.innerHeight - edge * 2);
      panel.style.width = `${width}px`;
      panel.style.maxHeight = `${maxHeight}px`;
      const height = Math.min(panel.getBoundingClientRect().height, maxHeight);
      const right = rect.right + edge;
      const left = right + width <= window.innerWidth - edge ? right : rect.left - width - edge;
      const x = Math.max(edge, Math.min(left, window.innerWidth - width - edge));
      const y = Math.max(edge, Math.min(rect.top, window.innerHeight - height - edge));
      const key = `${x},${y}`;
      if (key !== lastPosition) {
        panel.style.left = `${x}px`;
        panel.style.top = `${y}px`;
        lastPosition = key;
      }
      // Track transforms/reordering as well as scroll; only one open panel runs this loop.
      raf = requestAnimationFrame(position);
    };
    position();
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
    return () => {
      cancelAnimationFrame(raf);
      window.removeEventListener("keydown", onKey);
      panel.hidePopover();
    };
  });
</script>

<div bind:this={panel} id="loudness-analysis" class="loudness-popup" popover="manual"
  role="dialog" tabindex="-1" aria-labelledby="loudness-heading">
  <header>
    <h2 id="loudness-heading">響度分析 <span>· {track.name}</span></h2>
    <button type="button" class="close" aria-label="關閉響度分析" data-tooltip="關閉並停止量測。"
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
  .loudness-popup {
    position: fixed; inset: auto; margin: 0; padding: 16px; box-sizing: border-box;
    overflow: auto; background: var(--bg-panel); color: var(--text);
    border: 1px solid var(--border); border-radius: 6px; box-shadow: 0 8px 24px #0006;
    font: 13px/1.5 "IBM Plex Sans TC", "Segoe UI", system-ui, sans-serif;
  }
  .loudness-popup::backdrop { background: transparent; pointer-events: none; }
  header, footer { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  h2 { margin: 0; font-size: 15px; min-width: 0; overflow-wrap: anywhere; }
  h2 span { font-weight: 400; }
  .close { flex: none; width: 44px; height: 44px; border: 0; background: transparent; font-size: 24px; }
  .status { margin: 0 0 12px; color: var(--text-dim); font-size: 12px; }
  .loudness-values { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; padding: 4px 0 20px; }
  .metric { display: flex; flex-direction: column; align-items: center; gap: 2px; }
  .metric-label, .metric-window { color: var(--text-dim); font-size: 12px; }
  .metric-value { font: 600 27px/1.4 "IBM Plex Mono", Consolas, monospace; font-variant-numeric: tabular-nums; }
  dl { border-top: 1px solid var(--border); margin: 0; padding: 12px 0; }
  dl > div { display: flex; justify-content: space-between; gap: 12px; padding: 5px 0; }
  dt { color: var(--text-dim); }
  dd { margin: 0; white-space: nowrap; font-variant-numeric: tabular-nums; font-weight: 600; }
  dd span { font-weight: 400; }
  .reference { border-top: 1px solid var(--border); padding: 12px 0; }
  .reference h3 { display: flex; align-items: baseline; gap: 8px; margin: 0 0 8px; font-size: 12px; font-weight: 500; }
  .reference h3 span, .reference-targets span { color: var(--text-dim); font-size: 12px; font-weight: 400; }
  .reference-targets { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
  .reference-targets > div { display: flex; flex-direction: column; gap: 2px; }
  .reference strong { font-variant-numeric: tabular-nums; font-weight: 500; }
  .reference-peak { margin: 8px 0 0; font-size: 12px; }
  .reference .note { margin-top: 4px; }
  footer { border-top: 1px solid var(--border); padding-top: 12px; color: var(--text-dim); font-size: 12px; }
  footer button { min-height: 36px; }
  time { font-variant-numeric: tabular-nums; }
  .note { margin: 10px 0 0; color: var(--text-dim); font-size: 12px; }
  .error { color: var(--err); overflow-wrap: anywhere; }
</style>
