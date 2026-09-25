<script lang="ts">
  // Plugin 編輯面板(玻璃浮動視窗):host 端參數控制,原生視窗仍可另開。
  // 參數值來自 get_params,調整以 80ms 合批送 set_param,避免拖曳塞爆 IPC。
  import { engineCommand } from "./protocol-commands.generated";
  import { errorText } from "./errors";
  import type { ParamInfo, RackSlot } from "./types";

  let {
    slot,
    onBypass,
    onClose,
  }: {
    slot: RackSlot;
    onBypass: (slot: RackSlot) => void;
    onClose: () => void;
  } = $props();

  let dlg = $state<HTMLDialogElement | null>(null);
  let params = $state<ParamInfo[]>([]);
  let tab = $state<"params" | "info">("params");
  let err = $state("");
  let dx = $state(0);
  let dy = $state(0);
  let nativeOpened = false;
  const pending = new Map<number, number>();
  let flushTimer: ReturnType<typeof setTimeout> | undefined;

  $effect(() => {
    if (dlg && !dlg.open) dlg.showModal();
    void load();
    return () => {
      clearTimeout(flushTimer);
      flush();
      if (nativeOpened) {
        void engineCommand("close_editor", { instanceId: slot.instanceId }).catch(() => {});
      }
      dlg?.close();
    };
  });

  async function load() {
    err = "";
    try {
      params = (await engineCommand("get_params", { instanceId: slot.instanceId })).params;
    } catch (e) {
      err = errorText(e);
    }
  }

  function setParam(p: ParamInfo, value: number) {
    p.normalized = value;
    pending.set(p.paramId, value);
    clearTimeout(flushTimer);
    flushTimer = setTimeout(flush, 80);
  }

  function flush() {
    clearTimeout(flushTimer);
    if (pending.size === 0) return;
    const batch = [...pending];
    pending.clear();
    for (const [paramId, value] of batch) {
      engineCommand("set_param", { instanceId: slot.instanceId, paramId, value })
        .catch((e) => { err = errorText(e); });
    }
  }

  function openNative() {
    err = "";
    engineCommand("open_editor", { instanceId: slot.instanceId })
      .then(() => { nativeOpened = true; })
      .catch((e) => { err = errorText(e); });
  }

  function finish() {
    flush();
    if (nativeOpened) {
      void engineCommand("close_editor", { instanceId: slot.instanceId }).catch(() => {});
      nativeOpened = false;
    }
    onClose();
  }

  function startDrag(e: PointerEvent) {
    if ((e.target as HTMLElement).closest("button, input, select")) return;
    const startX = e.clientX - dx;
    const startY = e.clientY - dy;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    const move = (ev: PointerEvent) => {
      dx = ev.clientX - startX;
      dy = ev.clientY - startY;
    };
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  }
</script>

<dialog
  bind:this={dlg}
  class="plugineditor"
  style="translate: {dx}px {dy}px"
  onclose={finish}
>
  <header class="glassbar" role="toolbar" tabindex="-1" aria-label="{slot.name} 面板標題（可拖曳）" onpointerdown={startDrag}>
    <button
      class="power"
      class:off={slot.bypassed}
      aria-pressed={!slot.bypassed}
      aria-label={slot.bypassed ? `${slot.name} bypass 中(點此啟用)` : `${slot.name} 啟用中(點此 bypass)`}
      onclick={() => onBypass(slot)}
      data-tooltip={slot.bypassed
        ? "此 plugin 目前為 bypass;按下可恢復處理。"
        : "此 plugin 正在處理音訊;按下切換為 bypass。"}
    >⏻</button>
    <div class="titles">
      <strong>{slot.name}</strong>
      <span>VST3 · 延遲 {slot.latencySamples} samples</span>
    </div>
    <div class="tabs" role="group" aria-label="面板分頁">
      <button type="button" aria-pressed={tab === "params"} onclick={() => (tab = "params")}>參數</button>
      <button type="button" aria-pressed={tab === "info"} onclick={() => (tab = "info")}>資訊</button>
    </div>
    <button
      class="dialog-close"
      aria-label="關閉 {slot.name} 編輯面板"
      data-tooltip="關閉編輯面板;已開的原生視窗會一併關閉。"
      onclick={() => dlg?.close()}
    >×</button>
  </header>

  {#if tab === "params"}
    <div class="cards">
      {#each params as p (p.paramId)}
        <div class="card">
          <div class="cardhead">
            <span class="pname">{p.name}</span>
            <output>{Math.round(p.normalized * 100)}%</output>
          </div>
          <input
            type="range"
            min="0"
            max="1"
            step="0.001"
            value={p.normalized}
            aria-label={p.name}
            data-tooltip="{p.name};雙擊回復預設值。"
            oninput={(e) => setParam(p, Number(e.currentTarget.value))}
            ondblclick={() => setParam(p, p.default)}
          />
        </div>
      {:else}
        <p class="empty">此 plugin 未提供可調參數;可開啟原生視窗操作。</p>
      {/each}
    </div>
  {:else}
    <dl class="info">
      <dt>路徑</dt><dd>{slot.pluginPath}</dd>
      <dt>Instance</dt><dd>{slot.instanceId}</dd>
      <dt>延遲</dt><dd>{slot.latencySamples} samples(已納入 PDC)</dd>
      <dt>參數</dt><dd>{params.length} 項</dd>
    </dl>
  {/if}
  {#if err}<p class="editerr" role="alert">{err}</p>{/if}

  <footer class="glassfoot">
    <span class="hint">拖曳標題列可移動面板</span>
    <button type="button" onclick={openNative}
      data-tooltip="開啟 plugin 自帶的原生視窗(完整 GUI 與預設管理)。">原生視窗</button>
  </footer>
</dialog>

<style>
  .plugineditor {
    width: min(620px, 92vw);
    padding: 0;
    color: var(--text);
    background: color-mix(in srgb, var(--bg-panel) 72%, transparent);
    backdrop-filter: blur(18px) saturate(1.35);
    border: 1px solid #ffffff26;
    border-radius: 16px;
    box-shadow: 0 18px 60px #000c;
  }
  .plugineditor::backdrop {
    background: #0006;
  }
  .glassbar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 11px 14px 9px;
    border-bottom: 1px solid #ffffff17;
    cursor: grab;
    user-select: none;
    touch-action: none;
  }
  .glassbar:active { cursor: grabbing; }
  .titles {
    flex: 1;
    display: grid;
    line-height: 1.3;
    min-width: 0;
  }
  .titles strong {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .titles span {
    color: var(--text-dim);
    font-size: 11px;
  }
  .power {
    flex: none;
    width: 26px;
    height: 24px;
    padding: 0;
    border-radius: 6px;
    color: var(--ok);
    border: 1px solid color-mix(in srgb, var(--ok) 55%, transparent);
    background: transparent;
    cursor: pointer;
  }
  .power.off {
    color: var(--text-dim);
    border-color: var(--text-dim);
  }
  .tabs {
    display: flex;
    gap: 4px;
    padding: 3px;
    background: #ffffff10;
    border-radius: 999px;
  }
  .tabs button {
    border: 0;
    border-radius: 999px;
    padding: 4px 16px;
    font: inherit;
    font-size: 12px;
    color: var(--text-dim);
    background: transparent;
    cursor: pointer;
  }
  .tabs button[aria-pressed="true"] {
    color: #10141b;
    background: linear-gradient(120deg, var(--accent), #7cc0ff);
  }
  .cards {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 7px;
    max-height: 46vh;
    overflow: auto;
    padding: 12px 14px;
  }
  .card {
    display: grid;
    gap: 2px;
    padding: 7px 10px 5px;
    background: #ffffff0d;
    border: 1px solid #ffffff14;
    border-radius: 10px;
  }
  .cardhead {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
    font-size: 11px;
    color: var(--text-dim);
  }
  .pname {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .cardhead output {
    flex: none;
    font: 10px/1.4 var(--mono);
    color: #bcd7f5;
    background: color-mix(in srgb, var(--accent) 15%, transparent);
    border-radius: 999px;
    padding: 1px 8px;
  }
  .card input[type="range"] {
    width: 100%;
    height: 14px;
    accent-color: var(--accent);
    cursor: pointer;
  }
  .empty {
    grid-column: 1 / -1;
    margin: 4px 2px;
    color: var(--text-dim);
  }
  .info {
    display: grid;
    grid-template-columns: 70px 1fr;
    gap: 6px 12px;
    margin: 0;
    padding: 12px 16px;
    font-size: 12px;
  }
  .info dt { color: var(--text-dim); }
  .info dd {
    margin: 0;
    overflow-wrap: anywhere;
    user-select: text;
  }
  .editerr {
    margin: 0;
    padding: 0 14px 8px;
    color: var(--err);
    font-size: 12px;
  }
  .glassfoot {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 10px;
    padding: 8px 14px 12px;
  }
  .hint {
    flex: 1;
    color: var(--text-dim);
    font-size: 11px;
  }
  .glassfoot button {
    padding: 5px 14px;
    font: inherit;
    font-size: 12px;
    color: var(--text);
    background: #ffffff14;
    border: 1px solid #ffffff2e;
    border-radius: 999px;
    cursor: pointer;
  }
  .glassfoot button:hover {
    border-color: var(--text-dim);
  }
</style>
