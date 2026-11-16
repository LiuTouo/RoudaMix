<script lang="ts">
  // 使用教學獨立視窗(main.ts 以 ?window=tutorial 掛載;tray 選單建立)。
  // 純靜態內容:落地頁選 ASIO/WASAPI → 依序步驟 + 動畫示意(TutorialDemos)→ 完成頁。
  // 視窗外觀與行為承襲 LoudnessWindow:無框 + shadow、Esc 關閉、header 拖曳。
  import { onMount } from "svelte";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import TutorialDemos from "./TutorialDemos.svelte";
  import { tutorialSteps, type InterfaceVariant } from "./tutorialSteps";

  let variant = $state<InterfaceVariant | null>(null);
  let i = $state(0);
  let replay = $state(0);

  const steps = $derived(variant ? tutorialSteps(variant) : []);
  const step = $derived(steps[i] ?? null);
  const done = $derived(variant !== null && i >= steps.length);

  function choose(v: InterfaceVariant) {
    variant = v;
    i = 0;
  }
  function next() {
    if (i < steps.length) i += 1;
  }
  function prev() {
    if (i > 0) i -= 1;
  }
  function restart(other = false) {
    if (other && variant) variant = variant === "asio" ? "wasapi" : "asio";
    i = 0;
  }
  function closeWindow() {
    void getCurrentWindow().close().catch(() => {});
  }
  // 整窗可拖:header 空白處拖曳(鈕除外)。
  function startWindowDrag(event: PointerEvent) {
    if (event.button !== 0) return;
    if (event.target instanceof Element && event.target.closest("button")) return;
    event.preventDefault();
    void getCurrentWindow().startDragging().catch(() => {});
  }

  onMount(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== "Escape" || event.defaultPrevented) return;
      event.preventDefault();
      closeWindow();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="tutorial-window">
  {#if !variant}
    <!-- 落地頁:選音訊介面 -->
    <header class="step-head" onpointerdown={startWindowDrag}>
      <h2>RoudaMix 使用教學</h2>
      <button type="button" class="dialog-close" aria-label="關閉教學視窗" data-tooltip="關閉視窗。" onclick={closeWindow}>×</button>
    </header>
    <p class="lead">先選擇你的音訊介面，教學會依介面顯示對應的設定流程。</p>
    <div class="cards" role="group" aria-label="選擇音訊介面">
      <button type="button" class="card" onclick={() => choose("asio")}>
        <span class="card-name">ASIO</span>
        <span class="card-desc">錄音介面專屬驅動 · 低延遲</span>
      </button>
      <button type="button" class="card" onclick={() => choose("wasapi")}>
        <span class="card-name">WASAPI</span>
        <span class="card-desc">系統音訊 · 免 ASIO 硬體</span>
      </button>
    </div>
    <p class="hint">不確定的話：電腦內建音效卡或 USB 耳機選 WASAPI；有錄音介面就選 ASIO。</p>
  {:else if done}
    <!-- 完成頁 -->
    <header class="step-head" onpointerdown={startWindowDrag}>
      <h2>教學完成 🎉</h2>
      <button type="button" class="dialog-close" aria-label="關閉教學視窗" data-tooltip="關閉視窗。" onclick={closeWindow}>×</button>
    </header>
    <div class="done-body">
      <p>照著做就能開始混音了。遇到問題隨時從系統匣重看教學。</p>
      <div class="done-actions">
        <button type="button" onclick={() => restart(true)} data-tooltip={variant === "asio" ? "改看系統音訊流程。" : "改看 ASIO 流程。"}>
          改看{variant === "asio" ? "WASAPI" : "ASIO"}教學
        </button>
        <button type="button" class="primary" onclick={() => restart()}>重新開始</button>
        <button type="button" onclick={closeWindow}>關閉</button>
      </div>
    </div>
  {:else if step}
    <!-- 步驟頁 -->
    <header class="step-head" onpointerdown={startWindowDrag}>
      <h2 id="tutorial-step-title">{step.title}</h2>
      <span class="progress mono">{i + 1} / {steps.length}</span>
      <button type="button" class="dialog-close" aria-label="關閉教學視窗" data-tooltip="關閉視窗。" onclick={closeWindow}>×</button>
    </header>
    <div class="dots" aria-hidden="true">
      {#each steps as s, k (s.id)}
        <span class="dot" class:on={k === i} class:past={k < i}></span>
      {/each}
    </div>
    <div class="demo-wrap">
      <TutorialDemos demo={step.demo} replayKey={replay} />
      <button
        type="button"
        class="replay"
        aria-label="重播動畫"
        data-tooltip="重播此步驟動畫。"
        onclick={() => (replay += 1)}
      >↻ 重播</button>
    </div>
    <p class="body" aria-labelledby="tutorial-step-title">{step.body}</p>
    <footer>
      <button type="button" onclick={prev} disabled={i === 0}>上一步</button>
      <button type="button" class="primary" onclick={next}>{i === steps.length - 1 ? "完成" : "下一步"}</button>
    </footer>
  {/if}
</div>

<style>
  /* 樣式承襲 LoudnessWindow 語彙:surface 底 + hairline + 小圓角,視窗外框由
     Tauri(decorations:false + shadow)提供。 */
  .tutorial-window {
    box-sizing: border-box;
    height: 100vh;
    overflow: auto;
    display: flex;
    flex-direction: column;
    padding: 16px;
    background: var(--surface);
    color: var(--text);
    font: 12px/1.45 var(--font-ui);
  }
  /* 落地頁 */
  .lead { margin: 14px 0 12px; }
  .cards { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
  .card {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 6px;
    padding: 18px 16px;
    text-align: left;
  }
  .card-name { font: 600 16px/1.3 var(--font-ui); }
  .card-desc { color: var(--text-dim); }
  .hint { margin: 14px 0 0; color: var(--text-dim); }
  /* 步驟頁 */
  .step-head {
    display: flex;
    align-items: center;
    gap: 12px;
    margin: 0 -16px;
    padding: 0 16px 10px;
    border-bottom: 1px solid var(--border);
    cursor: grab;
    user-select: none;
  }
  h2 { margin: 0; font: 600 14px/1.45 var(--font-ui); letter-spacing: 0.3px; flex: 1 1 auto; min-width: 0; }
  .progress { color: var(--text-dim); font-size: 11px; }
  .dots { display: flex; gap: 6px; margin: 12px 0 10px; }
  .dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--border);
  }
  .dot.past { background: color-mix(in srgb, var(--text) 45%, var(--surface)); }
  .dot.on { background: var(--text); }
  .demo-wrap { position: relative; flex: 1 1 auto; display: flex; align-items: center; justify-content: center; }
  .replay {
    position: absolute;
    right: 0;
    bottom: 0;
    font-size: 11px;
    padding: 2px 10px;
  }
  .body { margin: 10px 0 12px; }
  footer { display: flex; justify-content: space-between; gap: 8px; margin-top: auto; padding-top: 8px; }
  /* 完成頁 */
  .done-body { display: flex; flex-direction: column; align-items: center; gap: 16px; margin: auto; text-align: center; }
  .done-actions { display: flex; gap: 8px; }
  /* 窄視窗:demo 畫布 420px 寬,中心對齊,兩側留白自然縮小 */
  @media (max-width: 460px) {
    .demo-wrap :global(.demo) { transform: scale(calc((100vw - 40px) / 420)); transform-origin: top center; }
  }
</style>
