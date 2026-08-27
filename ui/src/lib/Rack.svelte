<script lang="ts">
  import MeterCanvas from "./MeterCanvas.svelte";
  import type { MeterStrip, RackSlot } from "./types";

  let {
    rack,
    strips,
    focusId,
    onfocus,
    onbypass,
    onmove,
    onremove,
    onadd,
  }: {
    rack: RackSlot[];
    strips: MeterStrip[];
    focusId: number | null;
    onfocus: (id: number) => void;
    onbypass: (slot: RackSlot) => void;
    onmove: (id: number, delta: number) => void;
    onremove: (id: number) => void;
    onadd: () => void;
  } = $props();

  const stripOf = (id: number) => strips.find((s) => s.instanceId === id);
  const out = $derived(stripOf(0xffffffff));
</script>

<div class="rackcol">
  <div class="colhead">
    <span>Rack</span>
    <span class="dim mono">{rack.length}/15</span>
    <button class="add" onclick={onadd} title="掃描並加入 VST3">＋</button>
  </div>

  <div class="slots">
    {#each rack as slot, i (slot.instanceId)}
      <div
        class="slot"
        class:focused={focusId === slot.instanceId}
        class:bypassed={slot.bypassed}
      >
        <div
          class="row"
          role="button"
          tabindex="0"
          onclick={() => onfocus(slot.instanceId)}
          onkeydown={(e) => e.key === "Enter" && onfocus(slot.instanceId)}
        >
          <button
            class="b"
            class:on={slot.bypassed}
            title={slot.bypassed ? "取消 bypass" : "Bypass"}
            onclick={(e) => { e.stopPropagation(); onbypass(slot); }}
          >B</button>
          <span class="name" title={`${slot.name}\n${slot.pluginPath}`}>{slot.name}</span>
          <span class="idx mono">{i + 1}</span>
        </div>
        <MeterCanvas strip={stripOf(slot.instanceId)} height={26} />
        <div class="row ops">
          <button title="上移" disabled={i === 0} onclick={() => onmove(slot.instanceId, -1)}>▲</button>
          <button title="下移" disabled={i === rack.length - 1} onclick={() => onmove(slot.instanceId, 1)}>▼</button>
          <button class="del" title="移除" onclick={() => onremove(slot.instanceId)}>×</button>
        </div>
      </div>
    {:else}
      <div class="empty">
        <span class="dim">空的 rack</span>
        <button onclick={onadd}>掃描 VST3…</button>
      </div>
    {/each}
  </div>

  <div class="out">
    <div class="row"><span class="name">Engine 輸出</span></div>
    <MeterCanvas strip={out} height={26} />
  </div>
</div>

<style>
  .rackcol {
    display: flex;
    flex-direction: column;
    gap: 6px;
    width: 230px;
    flex-shrink: 0;
    overflow-y: auto;
  }
  .colhead {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: var(--text-dim);
  }
  .colhead .add {
    margin-left: auto;
    padding: 2px 9px;
  }
  .slots {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .slot {
    background: var(--bg-panel);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 5px 7px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .slot.focused {
    border-color: var(--accent);
  }
  .slot.bypassed .name {
    color: var(--text-dim);
    text-decoration: line-through;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .row[role="button"] {
    cursor: pointer;
  }
  .b {
    width: 22px;
    height: 20px;
    padding: 0;
    font-size: 11px;
    font-weight: 700;
    color: var(--text-dim);
  }
  .b.on {
    background: var(--warn);
    border-color: var(--warn);
    color: #14161a;
  }
  .name {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 12px;
  }
  .idx {
    color: var(--text-dim);
    font-size: 10px;
  }
  .ops {
    justify-content: flex-end;
  }
  .ops button {
    padding: 0 6px;
    font-size: 10px;
    height: 20px;
  }
  .del:hover {
    border-color: var(--err);
    color: var(--err);
  }
  .empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    padding: 24px 0;
    border: 1px dashed var(--border);
    border-radius: 6px;
  }
  .out {
    background: var(--bg-panel);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 5px 7px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    margin-top: auto;
  }
  .dim {
    color: var(--text-dim);
  }
  .mono {
    font-family: var(--mono);
  }
</style>
