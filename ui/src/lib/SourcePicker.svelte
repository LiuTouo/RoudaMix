<script lang="ts">
  // 來源選單:樹狀 popover(父層 ASIO / WASAPI),外觀同 PluginPicker 的樹
  // (chev hover 放大 + 展開旋轉);錨點定位/關閉行為同 DestinationSelect。
  import { onMount } from "svelte";
  import type { CaptureDevice } from "./types";

  let {
    trackId,
    trackName,
    /** 與舊 select 相同編碼:"" = 無、"N" = ASIO pair、"mN" = 單聲、"mic:<id|default>" = WASAPI */
    value,
    inputNames,
    maxIn,
    hasAsio,
    captureDevices,
    onOpen,
    onPick,
  }: {
    trackId: number;
    trackName: string;
    value: string;
    inputNames: string[];
    maxIn: number;
    hasAsio: boolean;
    captureDevices: CaptureDevice[];
    onOpen: () => void;
    onPick: (value: string) => void;
  } = $props();

  const panelId = $derived(`source-picker-${trackId}`);

  // ASIO pair 選項:基底偶數 ch,顯示「1/2 名稱」;無名稱時退回 maxIn 計數
  const pairs = $derived.by(() => {
    const opts: { value: string; label: string }[] = [];
    for (let ch = 0; ch + 1 < inputNames.length; ch += 2) {
      opts.push({ value: String(ch), label: `${ch + 1}/${ch + 2} ${inputNames[ch]}` });
    }
    if (opts.length === 0 && maxIn >= 2) {
      for (let ch = 0; ch + 1 < maxIn; ch += 2) opts.push({ value: String(ch), label: `${ch + 1}/${ch + 2}` });
    }
    return opts;
  });
  const monos = $derived(inputNames.map((nm, ch) => ({ value: `m${ch}`, label: `${ch + 1} ${nm}（單聲）` })));

  const summary = $derived.by(() => {
    if (value === "") return "(無)";
    if (value.startsWith("mic:")) {
      const id = value.slice(4);
      if (id === "default") return "系統預設麥克風";
      const d = captureDevices.find((c) => c.id === id);
      return d ? `${d.name}${d.default ? "（預設）" : ""}` : id;
    }
    const ch = value.startsWith("m") ? Number(value.slice(1)) : Number(value);
    if (!hasAsio) return `ASIO ${ch + 1}${value.startsWith("m") ? "（單聲）" : Number.isNaN(ch) ? "" : `/${ch + 2}`}（此機器無 ASIO 驅動）`;
    if (value.startsWith("m")) return inputNames[ch] ? `${ch + 1} ${inputNames[ch]}（單聲）` : `${ch + 1}（單聲）`;
    return pairs.find((p) => p.value === value)?.label ?? `${ch + 1}/${ch + 2}`;
  });

  let trigger: HTMLButtonElement;
  let panel: HTMLDivElement;
  let expanded = $state(false);

  function positionPanel() {
    if (!trigger || !panel) return;
    const rect = trigger.getBoundingClientRect();
    const gap = 5;
    const edge = 8;
    const width = Math.min(Math.max(rect.width, 260), window.innerWidth - edge * 2);
    const below = Math.max(0, window.innerHeight - rect.bottom - gap - edge);
    const above = Math.max(0, rect.top - gap - edge);
    const downward = below >= above;
    panel.style.width = `${width}px`;
    panel.style.maxHeight = `${downward ? below : above}px`;
    panel.style.left = `${Math.max(edge, Math.min(rect.left, window.innerWidth - width - edge))}px`;
    panel.style.top = downward ? `${rect.bottom + gap}px` : "auto";
    panel.style.bottom = downward ? "auto" : `${window.innerHeight - rect.top + gap}px`;
  }

  function beforeToggle(event: ToggleEvent) {
    expanded = event.newState === "open";
    if (expanded) positionPanel();
  }

  function closePanel(restoreFocus = false) {
    if (expanded) panel.hidePopover();
    if (restoreFocus) trigger.focus({ preventScroll: true });
  }

  function pick(next: string) {
    onPick(next);
    closePanel(true);
  }

  onMount(() => {
    const onScroll = (event: Event) => {
      if (!(event.target instanceof Node) || !panel.contains(event.target)) closePanel();
    };
    const onFocus = (event: FocusEvent) => {
      if (event.target instanceof Node && !panel.contains(event.target) && !trigger.contains(event.target)) closePanel();
    };
    window.addEventListener("resize", positionPanel);
    window.addEventListener("scroll", onScroll, true);
    document.addEventListener("focusin", onFocus);
    return () => {
      window.removeEventListener("resize", positionPanel);
      window.removeEventListener("scroll", onScroll, true);
      document.removeEventListener("focusin", onFocus);
      closePanel();
    };
  });
</script>

<div class="source-picker">
  <button
    bind:this={trigger}
    class="source-trigger"
    type="button"
    popovertarget={panelId}
    aria-haspopup="dialog"
    aria-expanded={expanded}
    aria-controls={panelId}
    aria-label="{trackName} 輸入來源：{summary}"
    data-tooltip="ASIO 輸入或麥克風（WASAPI）；清單點開時載入。"
    onclick={onOpen}
  >
    <span class="source-summary">{summary}</span>
    <svg width="10" height="7" viewBox="0 0 10 7" aria-hidden="true"><path d="m1 1.5 4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.5" /></svg>
  </button>

  <div
    bind:this={panel}
    id={panelId}
    class="source-popup"
    popover="auto"
    role="dialog"
    tabindex="-1"
    aria-label="{trackName} 的輸入來源（樹狀選單）"
    onbeforetoggle={beforeToggle}
    ontoggle={(event) => {
      if (event.newState === "open") {
        (panel.querySelector<HTMLButtonElement>("button.sel") ?? panel.querySelector<HTMLButtonElement>("button"))?.focus({ preventScroll: true });
      }
    }}
    onkeydown={(event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        closePanel(true);
      }
    }}
  >
    <p class="hint">輸入來源 · 單選</p>
    <div class="tree">
      <button class="src-row" type="button" data-value="" class:sel={value === ""} onclick={() => pick("")}>
        <span class="row-label">(無)</span>
      </button>

      {#if hasAsio}
        <details class="src-group" open={!value.startsWith("mic:")}>
          <summary>
            <span class="chev" aria-hidden="true"></span>
            <span class="tree-label">ASIO</span>
            <span class="tree-count">{pairs.length + monos.length}</span>
          </summary>
          <div class="kids">
            {#each pairs as o (o.value)}
              <button class="src-row" type="button" data-value={o.value} class:sel={value === o.value} onclick={() => pick(o.value)}>
                <span class="row-label">{o.label}</span>
                <span class="row-kind">pair</span>
              </button>
            {/each}
            {#each monos as o (o.value)}
              <button class="src-row" type="button" data-value={o.value} class:sel={value === o.value} onclick={() => pick(o.value)}>
                <span class="row-label">{o.label}</span>
                <span class="row-kind">單聲</span>
              </button>
            {/each}
          </div>
        </details>
      {:else if !value.startsWith("mic:") && value !== ""}
        <!-- M6:純 WASAPI 機器載入 ASIO 來源 session:來源保留但靜音 -->
        <p class="noasio">ASIO {Number(value.replace("m", "")) + 1}（此機器無 ASIO 驅動，來源保留但靜音）</p>
      {/if}

      <details class="src-group" open={value.startsWith("mic:")}>
        <summary>
          <span class="chev" aria-hidden="true"></span>
          <span class="tree-label">WASAPI</span>
          <span class="tree-count">{captureDevices.length + 1}</span>
        </summary>
        <div class="kids">
          <button class="src-row" type="button" data-value="mic:default" class:sel={value === "mic:default"} onclick={() => pick("mic:default")}>
            <span class="row-label">系統預設麥克風</span>
          </button>
          {#each captureDevices as d (d.id)}
            <button class="src-row" type="button" data-value={`mic:${d.id}`} class:sel={value === `mic:${d.id}`} onclick={() => pick(`mic:${d.id}`)}>
              <span class="row-label">{d.name}</span>
              {#if d.default}<span class="row-kind">預設</span>{/if}
            </button>
          {/each}
        </div>
      </details>
    </div>
  </div>
</div>

<style>
  .source-picker { flex: 1; min-width: 0; }
  .source-trigger {
    width: 100%; min-width: 0; display: flex; align-items: center; gap: 5px;
    padding: 3px 6px; font-size: 12px; line-height: normal;
    background: var(--bg); color: var(--text); border: 1px solid var(--border);
    border-radius: var(--flat-radius); text-align: left;
  }
  .source-trigger[aria-expanded="true"] { border-color: var(--accent); }
  .source-trigger svg { flex: 0 0 10px; }
  .source-summary { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  .source-popup {
    position: fixed; inset: auto; margin: 0; padding: 6px; box-sizing: border-box;
    overflow: auto;
    /* 底色/框線/圓角由 console.css 浮層規則統一 */
    color: var(--text);
    font: 12px/1.45 var(--font-ui);
  }
  .source-popup::backdrop { background: transparent; }
  .hint { margin: 0; padding: 3px 6px 7px; color: var(--text-dim); font-size: 11px; }

  /* ---- 樹狀目錄(同 PluginPicker:chev hover 放大、展開旋轉) ---- */
  summary { cursor: pointer; }
  .src-group summary {
    display: flex; align-items: center; gap: 8px; padding: 4px 8px;
    list-style: none; border-radius: var(--flat-radius);
    color: var(--text-dim); font-size: 12px;
  }
  summary::-webkit-details-marker { display: none; }
  .chev {
    flex: none; width: 0; height: 0;
    border-top: 4px solid transparent; border-bottom: 4px solid transparent;
    border-left: 5px solid currentColor;
    transition: transform 0.12s ease;
  }
  details[open] > summary .chev { transform: rotate(90deg); }
  summary:hover .chev { transform: scale(1.5); }
  details[open] > summary:hover .chev { transform: rotate(90deg) scale(1.5); }
  summary:hover .tree-label { color: var(--text); }
  .tree-label { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .tree-count { flex: none; font-size: 11px; opacity: 0.7; }
  .kids { margin-left: 10px; padding-left: 8px; border-left: 1px solid var(--border); }

  .src-row {
    display: flex; width: 100%; align-items: center; gap: 8px;
    text-align: left; padding: 5px 8px; font-size: 13px; color: var(--text);
    background: transparent; border: 0; border-radius: var(--flat-radius);
  }
  .row-label { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .row-kind { flex: none; font-size: 10px; padding: 1px 6px; border: 1px solid var(--border); border-radius: var(--flat-radius); color: var(--text-dim); }
  /* 反白選取;!important 壓過全域主題對按鈕的底色設定 */
  :global(#app) .sel {
    background: var(--accent) !important; border-color: var(--accent); color: var(--flat-0) !important;
  }
  .sel .row-kind { color: inherit; border-color: color-mix(in srgb, var(--flat-0) 40%, transparent); }
  .noasio { margin: 4px 8px; color: var(--warn); font-size: 12px; }
</style>
