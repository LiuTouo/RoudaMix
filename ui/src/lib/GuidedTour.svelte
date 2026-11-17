<script lang="ts">
  // 引導式教學：spotlight 遮罩（四矩形蓋洞）+ 對話框箭頭指向真實控制項。
  // read 步驟按「下一步」前進；click 步驟實際點擊高亮目標前進（下一步可代行）。
  // 設定 dialog 於教學中由 App 改以非 modal show() 開啟（不進 top-layer），
  // 遮罩與卡片直接以 z-index 覆蓋，避免 portal 與 dialog transform/overflow 陷阱。
  import { guidedTourSteps, type InterfaceVariant, type TourStep } from "./guidedTour";

  let {
    tourTrackId = () => null,
    hasInputTracks = () => false,
    settingsOpen = false,
    resolveTimeoutMs = 3000,
    onAddTrack,
    onEnsureTrack,
    onOpenVst,
    onOpenSettings,
    onCloseSettings,
    onEnd,
  }: {
    /** 回傳教學示範軌 id（優先 audio，否則第一條輸入軌）；全空 = null。 */
    tourTrackId?: () => number | null;
    /** 輸入帶是否已有軌 → 有就沿用現有軌，跳過「新增音軌」步驟。 */
    hasInputTracks?: () => boolean;
    settingsOpen?: boolean;
    /** 目標等待上限；測試縮短用。 */
    resolveTimeoutMs?: number;
    onAddTrack: () => void;
    onEnsureTrack: (id: number) => void;
    onOpenVst: (id: number) => void;
    onOpenSettings: (tab: "audio" | "scenes") => void;
    onCloseSettings: () => void;
    onEnd: () => void;
  } = $props();

  let variant = $state<InterfaceVariant | null>(null);
  let i = $state(0);
  let trackId = $state<number | null>(null); // 凍結的示範軌 id
  let targetEl = $state<HTMLElement | null>(null);
  let spotlight = $state<{ x: number; y: number; w: number; h: number } | null>(null);
  let cardPos = $state<{ x: number; y: number; below: boolean } | null>(null);
  let arrowX = $state(0);
  // 到位才顯示：換步時卡片先隱藏量測，位置就緒同一帧才現身（不經過置中再跳）
  let placed = $state(false);

  let cardEl = $state<HTMLDivElement | null>(null);
  let headingEl = $state<HTMLElement | null>(null);

  const steps = $derived(variant ? guidedTourSteps(variant) : []);
  const step = $derived(steps[i] ?? null);
  const done = $derived(variant !== null && i >= steps.length);

  let gen = 0; // 世代計數：換步/結束時讓進行中的 resolveLoop 自行放棄
  let settingsWereOpen: boolean | null = null;
  let prevFocus: Element | null = null;

  const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));
  const raf = () => new Promise<void>((r) => requestAnimationFrame(() => r()));

  function choose(v: InterfaceVariant) {
    variant = v;
    trackId = null;
    goTo(0);
  }

  /** 離開現步的副作用（關設定），再進入下一步。 */
  function advance(byClick: boolean) {
    if (!step) return;
    // click 步驟按「下一步」= 代行：開設定（下一步 opens 會接手）、新增音軌。
    if (step.mode === "click" && !byClick && step.id === "add-track" && currentTrackId() == null)
      onAddTrack();
    if (step.closesSettings) onCloseSettings();
    goTo(i + 1);
  }

  function goTo(index: number) {
    gen++;
    i = index;
    targetEl = null;
    placed = false; // 舊遮罩/舊位置保持到新位置就緒，避免全暗脈衝與卡片彈跳
    if (index < steps.length) void enter(steps[index], gen);
  }

  async function enter(s: TourStep, myGen: number) {
    if (s.opensSettings) onOpenSettings(s.opensSettings);
    // 輸入帶已有軌 → 沿用現有軌，跳過「新增音軌」（使用者指定的行為）
    if (s.id === "add-track" && hasInputTracks()) {
      goTo(i + 1);
      return;
    }
    const deadline = performance.now() + resolveTimeoutMs;
    let vstAsked = false;
    const bindTrack = (id: number) => {
      if (trackId == null) {
        trackId = id; // 凍結示範軌
        onEnsureTrack(id); // 捲入 lane 虛擬化窗口（best effort）
      }
    };
    while (performance.now() < deadline) {
      if (myGen !== gen) return;
      const id = currentTrackId();
      if (s.selector.includes("{track}")) {
        if (id == null) {
          await sleep(120); // 引擎建立音軌的往返等待
          continue;
        }
        bindTrack(id);
        if (s.id === "track-plugin" && !vstAsked) {
          onOpenVst(id); // 插件槽在 VST 面板內（收闔時寬度 0），先展開
          vstAsked = true;
        }
      }
      const el = document.querySelector<HTMLElement>(s.selector.replace("{track}", String(id ?? -1)));
      // 目標在 dialog 內時必須等 dialog 真的 open（App 在教學中改用非 modal show()，
      // 但仍是 Svelte flush 後才開）：太早接受 = 目標還不可見、遮罩打在空處。
      const dlg = el?.closest("dialog") ?? null;
      const ready = !!el && el.isConnected && (!dlg || dlg.open);
      if (ready && el) {
        if (myGen !== gen) return;
        targetEl = el;
        el.scrollIntoView({ block: "nearest", inline: "nearest" });
        await raf();
        await raf(); // 捲動 → 虛擬化重排後再量測
        if (myGen !== gen) return;
        layout();
        // 分頁 fade/VST 面板展開等動畫結束後再補一次排版
        setTimeout(() => {
          if (myGen === gen) layout();
        }, 300);
        return;
      }
      await sleep(120);
    }
    // 超時後最後再試一次:等音軌建立的引擎往返常在 deadline 邊緣抵達。
    // 一律不自動跳步：等不到就維持置中卡，由使用者手動「下一步」離開。
    {
      const id = currentTrackId();
      if (id != null && s.selector.includes("{track}")) bindTrack(id);
      const el = id != null
        ? document.querySelector<HTMLElement>(s.selector.replace("{track}", String(id)))
        : null;
      const dlg = el?.closest("dialog") ?? null;
      if (el && el.isConnected && (!dlg || dlg.open) && myGen === gen) {
        targetEl = el;
        layout();
      }
    }
    // 等不到目標：收回 spotlight 全暗 + 置中卡現身，不留在舊位置誤導
    if (myGen === gen && !targetEl) {
      spotlight = null;
      cardPos = null;
      placed = true;
    }
  }

  function currentTrackId(): number | null {
    return trackId ?? tourTrackId() ?? null;
  }

  function layout() {
    const el = targetEl;
    if (!el || !el.isConnected) {
      spotlight = null;
      cardPos = null;
      return;
    }
    const r = el.getBoundingClientRect();
    const pad = 6;
    spotlight = {
      x: Math.max(0, r.left - pad),
      y: Math.max(0, r.top - pad),
      w: Math.min(r.width + pad * 2, window.innerWidth),
      h: Math.min(r.height + pad * 2, window.innerHeight),
    };
  }

  // 卡片放置：偏好目標下方，空間不足翻上；左右夾限；箭頭對齊目標中心。
  $effect(() => {
    if (!cardEl || !spotlight) return;
    void i; // 換步內文高度變 → 重放
    const card = cardEl;
    void spotlight;
    const cw = card.offsetWidth || 1;
    const ch = card.offsetHeight || 1;
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    const gap = 12;
    const edge = 8;
    const cx = spotlight.x + spotlight.w / 2;
    const below = spotlight.y + spotlight.h + gap + ch <= vh - edge;
    const x = Math.min(Math.max(cx - cw / 2, edge), Math.max(edge, vw - cw - edge));
    const y = below
      ? spotlight.y + spotlight.h + gap
      : Math.max(edge, spotlight.y - ch - gap);
    arrowX = Math.min(Math.max(cx - x, 14), Math.max(14, cw - 14));
    cardPos = { x, y, below };
    placed = true; // 位置就緒 → 同帧現身（遮罩洞也在同一次繪製切換）
  });

  // 焦點：換頁聚焦卡片標題；結束還原進教學前的焦點
  $effect(() => {
    void i;
    void variant;
    void done;
    headingEl?.focus({ preventScroll: true });
  });

  function end() {
    gen++;
    if (!settingsWereOpen && settingsOpen) onCloseSettings();
    (prevFocus instanceof HTMLElement ? prevFocus : null)?.focus?.({ preventScroll: true });
    onEnd();
  }

  function restart(other = false) {
    if (other && variant) variant = variant === "asio" ? "wasapi" : "asio";
    trackId = null;
    goTo(0);
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      e.preventDefault(); // 先擋：設定 dialog 的 Esc-cancel 不接手，教學統一收尾
      end();
      return;
    }
    if (e.key !== "Tab" || !cardEl) return;
    // 簡易焦點陷阱：Tab 在卡片控制項間循環（遮罩後背景不可達）
    const focusables = [...cardEl.querySelectorAll<HTMLElement>("button, [href], input, select, textarea")].filter(
      (el) => !el.hasAttribute("disabled"),
    );
    if (focusables.length === 0) return;
    const first = focusables[0];
    const last = focusables[focusables.length - 1];
    const active = document.activeElement;
    if (e.shiftKey && (active === first || !cardEl.contains(active))) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (active === last || !cardEl.contains(active))) {
      e.preventDefault();
      first.focus();
    }
  }

  // 互動鎖：點擊只放行「卡片內」與「click 步驟的目標」，其餘吞掉（含 dialog 內容）
  function allowTarget(t: Node): boolean {
    return step?.mode === "click" && !!targetEl && targetEl.contains(t);
  }
  function swallowPointer(e: Event) {
    const t = e.target as Node | null;
    if (!t || !variant || done) return;
    if (cardEl?.contains(t) || allowTarget(t)) return;
    e.preventDefault();
    e.stopPropagation();
  }
  function onDocClick(e: MouseEvent) {
    const t = e.target as Node | null;
    if (!t || !step || done) return;
    if (cardEl?.contains(t)) return;
    if (allowTarget(t)) {
      // 延後一拍再換步：capture 内同步 flush 會拆掉目標節點，
      // Svelte 5 事件委派的 onclick（掛在 root）就收不到 = 點了沒反應。
      const myGen = gen;
      setTimeout(() => {
        if (myGen === gen) advance(true);
      }, 0);
      return;
    }
    e.preventDefault();
    e.stopPropagation();
  }

  $effect(() => {
    if (settingsWereOpen === null) settingsWereOpen = settingsOpen; // 只取進入教學當下的值
    prevFocus ??= document.activeElement;
    window.addEventListener("resize", layout);
    window.addEventListener("scroll", layout, true);
    document.addEventListener("pointerdown", swallowPointer, true);
    document.addEventListener("click", onDocClick, true);
    window.addEventListener("keydown", onKeydown, true);
    return () => {
      window.removeEventListener("resize", layout);
      window.removeEventListener("scroll", layout, true);
      document.removeEventListener("pointerdown", swallowPointer, true);
      document.removeEventListener("click", onDocClick, true);
      window.removeEventListener("keydown", onKeydown, true);
    };
  });

</script>

<div class="tour-root">
  {#if variant === null}
    <!-- 落地頁：選音訊介面 -->
    <div class="tour-veil" aria-hidden="true"></div>
    <div
      class="tour-card tour-center"
      role="dialog"
      aria-modal="true"
      aria-labelledby="tour-heading"
      bind:this={cardEl}
    >
      <div class="tour-head">
        <h3 id="tour-heading" tabindex="-1" bind:this={headingEl}>RoudaMix 使用教學</h3>
        <button type="button" class="dialog-close" aria-label="關閉教學" data-tooltip="結束教學。" onclick={end}>×</button>
      </div>
      <p>先選擇你的音訊介面，教學會依介面顯示對應的設定流程。</p>
      <div class="tour-cards" role="group" aria-label="選擇音訊介面">
        <button type="button" class="tour-card-choice" onclick={() => choose("asio")}>
          <span class="name">ASIO</span>
          <span class="desc">錄音介面專屬驅動 · 低延遲</span>
        </button>
        <button type="button" class="tour-card-choice" onclick={() => choose("wasapi")}>
          <span class="name">WASAPI</span>
          <span class="desc">系統音訊 · 免 ASIO 硬體</span>
        </button>
      </div>
      <p class="tour-dim">不確定的話：電腦內建音效卡或 USB 耳機選 WASAPI；有錄音介面就選 ASIO。</p>
    </div>
  {:else if done}
    <!-- 完成頁 -->
    <div class="tour-veil" aria-hidden="true"></div>
    <div
      class="tour-card tour-center"
      role="dialog"
      aria-modal="true"
      aria-labelledby="tour-heading"
      bind:this={cardEl}
    >
      <div class="tour-head">
        <h3 id="tour-heading" tabindex="-1" bind:this={headingEl}>教學完成 🎉</h3>
        <button type="button" class="dialog-close" aria-label="關閉教學" data-tooltip="結束教學。" onclick={end}>×</button>
      </div>
      <p>照著做就能開始混音了。遇到問題隨時從系統匣重看教學。</p>
      <footer class="tour-actions">
        <button
          type="button"
          onclick={() => restart(true)}
          data-tooltip={variant === "asio" ? "改看系統音訊流程。" : "改看 ASIO 流程。"}
          >改看{variant === "asio" ? "WASAPI" : "ASIO"}教學</button
        >
        <button type="button" class="primary" onclick={() => restart()}>重新開始</button>
        <button type="button" onclick={end}>關閉</button>
      </footer>
    </div>
  {:else if step}
    <!-- 進行中：四矩形遮罩 + spotlight 對話框（設定在教學中改非 modal，z-index 即可覆蓋） -->
    <div class="tour-mask" aria-hidden="true">
        {#if spotlight}
          <div class="tour-block" style="left:0; top:0; right:0; height:{spotlight.y}px"></div>
          <div class="tour-block" style="left:0; top:{spotlight.y}px; width:{spotlight.x}px; height:{spotlight.h}px"></div>
          <div
            class="tour-block"
            style="left:{spotlight.x + spotlight.w}px; top:{spotlight.y}px; right:0; height:{spotlight.h}px"
          ></div>
          <div class="tour-block" style="left:0; top:{spotlight.y + spotlight.h}px; right:0; bottom:0"></div>
        {:else}
          <div class="tour-block" style="inset:0"></div>
        {/if}
    </div>
    <div class="tour-pop">
      {#if spotlight}
        <div
          class="tour-ring"
          aria-hidden="true"
          style="left:{spotlight.x}px; top:{spotlight.y}px; width:{spotlight.w}px; height:{spotlight.h}px"
        ></div>
      {/if}
      <div
        class="tour-card"
        class:gating={!placed}
        class:placed={!!cardPos}
        class:arrow-up={cardPos && !cardPos.below}
        class:centered={!cardPos}
        style={cardPos ? `left:${cardPos.x}px; top:${cardPos.y}px; --arrow-x:${arrowX}px` : undefined}
        role="dialog"
        aria-modal="true"
        aria-labelledby="tour-heading"
        bind:this={cardEl}
      >
        <div class="tour-head">
          <h3 id="tour-heading" tabindex="-1" bind:this={headingEl}>{step.title}</h3>
          <span class="tour-prog mono">{i + 1} / {steps.length}</span>
          <button type="button" class="dialog-close" aria-label="結束教學" data-tooltip="結束教學。" onclick={end}>×</button>
        </div>
        <p>{step.body}</p>
        {#if step.mode === "click"}
          <p class="tour-click-hint">點擊高亮處繼續</p>
        {/if}
        <footer class="tour-actions">
          <button type="button" onclick={() => goTo(i - 1)} disabled={i === 0}>上一步</button>
          <button type="button" class="primary" onclick={() => advance(false)}
            >{i === steps.length - 1 ? "完成" : "下一步"}</button
          >
        </footer>
      </div>
    </div>
  {/if}
</div>

<style>
  .tour-veil {
    position: fixed;
    inset: 0;
    z-index: 300;
    background: #0009;
  }
  /* 四矩形遮罩：蓋住洞外一切（pointer 命中遮罩即吞掉；document capture 再補一層） */
  .tour-mask {
    position: fixed;
    inset: 0;
    z-index: 300;
  }
  .tour-block {
    position: absolute;
    background: #0009;
  }
  .tour-pop {
    position: fixed;
    inset: 0;
    z-index: 301;
    pointer-events: none;
  }
  .tour-ring {
    position: absolute;
    border-radius: 6px;
    box-shadow: 0 0 0 2px var(--text);
    pointer-events: none;
  }
  .tour-card {
    position: absolute;
    box-sizing: border-box;
    width: min(320px, calc(100vw - 16px));
    padding: 12px 14px;
    border: 1px solid var(--border);
    border-radius: var(--flat-radius, 8px);
    background: var(--surface);
    box-shadow: 0 14px 36px #000b;
    color: var(--text);
    font: 12px/1.5 var(--font-ui);
    pointer-events: auto;
  }
  .tour-card.gating {
    /* 量測中不現身：避免「置中→目標」的彈跳 */
    visibility: hidden;
  }
  .tour-card.placed::before {
    /* 箭頭：對話框在上 → 指上；arrow-up 反之。--arrow-x 由 style 帶入 */
    content: "";
    position: absolute;
    top: -7px;
    left: var(--arrow-x);
    margin-left: -7px;
    border: 7px solid transparent;
    border-top: 0;
    border-bottom: 8px solid var(--surface);
    filter: drop-shadow(0 -1px 0 var(--border));
  }
  .tour-card.arrow-up::before {
    top: auto;
    bottom: -7px;
    border: 7px solid transparent;
    border-bottom: 0;
    border-top: 8px solid var(--surface);
    filter: drop-shadow(0 1px 0 var(--border));
  }
  .tour-card.centered,
  .tour-card.tour-center {
    position: fixed;
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    z-index: 301;
  }
  .tour-head {
    display: flex;
    align-items: center;
    gap: 10px;
    margin: 0 0 8px;
  }
  /* 使用者要求：教學卡不顯示 TAB 焦點框（Tab 焦點移動仍在，僅不畫外框） */
  .tour-card :is(:focus-visible, :focus),
  .tour-card:is(:focus-visible, :focus) {
    outline: none;
  }
  h3 {
    flex: 1 1 auto;
    min-width: 0;
    margin: 0;
    font: 600 13px/1.4 var(--font-ui);
  }
  .tour-prog {
    color: var(--text-dim);
    font-size: 11px;
  }
  p {
    margin: 0 0 10px;
  }
  .tour-dim {
    color: var(--text-dim);
  }
  .tour-click-hint {
    margin: -4px 0 10px;
    color: var(--text-dim);
    font-size: 11px;
  }
  .tour-actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
  }
  .tour-cards {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 10px;
    margin: 0 0 10px;
  }
  .tour-card-choice {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 4px;
    padding: 14px 12px;
    text-align: left;
  }
  .tour-card-choice .name {
    font: 600 15px/1.2 var(--font-ui);
  }
  .tour-card-choice .desc {
    color: var(--text-dim);
  }
</style>
