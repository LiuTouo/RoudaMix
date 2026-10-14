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
  const statusText = $derived(view.busy ? "正在開始量測…" : interrupted ? "量測中斷（非即時）" :
    !running ? (report ? "已停止，數值已凍結" : "引擎未啟動") :
    report?.state === "stopped" ? "已停止，數值已凍結" :
    !report || report.elapsedSeconds < 3 ? "收集資料中…" : "量測中");
  const metrics = [
    { key: "momentaryLufs", label: "LUFS-M", window: "400 ms", help: "最近 400 毫秒的瞬時響度，用來觀察當下音量變化。" },
    { key: "shortTermLufs", label: "LUFS-S", window: "3 秒", help: "最近 3 秒的短期響度，用來比較一小段聲音的持續響度。" },
    { key: "integratedLufs", label: "LUFS-I", window: "本次累積", help: "自開啟或重置起、經 gating 的整體響度；沒有有效資料時顯示 —。" },
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
    <button type="button" class="close" aria-label="關閉響度分析" data-tooltip="關閉並停止本次響度量測。"
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
    <div><dt data-tooltip="自開啟或重置起，左右聲道中最高的真峰值；包含取樣點之間可能出現的峰值，不等同 Peak／dBFS。">最高 True Peak</dt>
      <dd data-metric="truePeakDbtp">{loudnessValue(report, "truePeakDbtp")} <span>dBTP</span></dd></div>
    <div><dt data-tooltip="LRA 描述這段聲音的響度變化範圍，不是峰值與 RMS 的差。剛開始或有效資料不足時不宜據此下結論。">Loudness Range（LRA）</dt>
      <dd data-metric="loudnessRangeLu">{loudnessValue(report, "loudnessRangeLu")} <span>LU</span></dd></div>
  </dl>
  {#if interrupted}<p class="error" role="alert">{view.error || report?.error || "量測中斷，請重置後再試。"}</p>{/if}
  {#if view.restarted && !view.busy}<p class="note">引擎重新啟動或取樣率已變更，量測已重置。</p>{/if}
  <footer>
    <span>已量測 <time>{loudnessDuration(report?.elapsedSeconds ?? 0)}</time></span>
    <button type="button" disabled={!running || view.busy} onclick={onReset}
      aria-label="重置響度量測" data-tooltip="清空五項量測、累積統計與計時，從現在重新開始。">{!report && view.error ? "重試量測" : "重置量測"}</button>
  </footer>
  <p class="note">累積值自開啟或上次重置起。— 表示收集資料中或有效資料不足；−∞ 表示靜音。</p>
  <p class="note">{track.kind === "output" ? "量測實際輸出 bus（含監聽分支），在送往裝置前取樣。" : "量測主訊號：插件、音量與靜音控制之後。"}不同用途的適合響度不同，數值不是音質合格判定。</p>
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
  footer { border-top: 1px solid var(--border); padding-top: 12px; color: var(--text-dim); font-size: 12px; }
  footer button { min-height: 36px; }
  time { font-variant-numeric: tabular-nums; }
  .note { margin: 10px 0 0; color: var(--text-dim); font-size: 12px; }
  .error { color: var(--err); overflow-wrap: anywhere; }
</style>
