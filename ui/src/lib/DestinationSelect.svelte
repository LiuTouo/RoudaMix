<script lang="ts">
  import { onMount } from "svelte";
  import { cssColor } from "./tracks";
  import type { Track } from "./types";

  let { trackId, trackName, options, selected, pending = false, onToggle }: {
    trackId: number;
    trackName: string;
    options: Pick<Track, "trackId" | "name" | "color">[];
    selected: number[];
    pending?: boolean;
    onToggle: (id: number, checked: boolean) => void;
  } = $props();

  const panelId = $derived(`track-destinations-${trackId}`);
  const names = $derived(selected.map((id) => options.find((option) => option.trackId === id)?.name ?? `#${id}`));
  const summary = $derived(names.length ? names.join("、") : "(無)");
  let trigger: HTMLButtonElement;
  let panel: HTMLDivElement;
  let expanded = $state(false);

  function positionPanel() {
    if (!trigger || !panel) return;
    const rect = trigger.getBoundingClientRect();
    const gap = 5;
    const edge = 8;
    const width = Math.min(Math.max(rect.width, 240), window.innerWidth - edge * 2);
    const below = Math.max(0, window.innerHeight - rect.bottom - gap - edge);
    const above = Math.max(0, rect.top - gap - edge);
    const preferred = Math.min(320, 44 + options.length * 30);
    const downward = below >= preferred || below >= above;
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

  function onPanelKeydown(event: KeyboardEvent) {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      closePanel(true);
      return;
    }
    if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    const inputs = [...panel.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')];
    if (!inputs.length) return;
    event.preventDefault();
    const index = inputs.indexOf(document.activeElement as HTMLInputElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? inputs.length - 1
      : (index + (event.key === "ArrowDown" ? 1 : -1) + inputs.length) % inputs.length;
    inputs[next].focus();
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

<div class="destination-control">
  <button
    bind:this={trigger}
    class="destination-trigger"
    type="button"
    popovertarget={panelId}
    aria-haspopup="dialog"
    aria-expanded={expanded}
    aria-controls={panelId}
    aria-label={`${trackName} 的輸出到：${summary}`}
    disabled={options.length === 0}
    data-tooltip={options.length === 0 ? "沒有可接收路由的 FX 或輸出軌。" : `輸出到：${names.length ? names.join("、") : "未選擇"}。可勾選多個目的地，立即套用。`}
  >
    <span class="destination-summary">{options.length === 0 ? "(無可用目的地)" : summary}</span>
    {#if selected.length > 1}<span class="destination-count" aria-hidden="true">{selected.length}</span>{/if}
    {#if pending}<span class="destination-pending" aria-label="路由套用中">…</span>{/if}
    <svg width="10" height="7" viewBox="0 0 10 7" aria-hidden="true"><path d="m1 1.5 4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.5" /></svg>
  </button>

  <div
    bind:this={panel}
    id={panelId}
    class="destination-popup"
    popover="auto"
    role="dialog"
    tabindex="-1"
    aria-label={`${trackName} 的輸出目的地（可多選）`}
    onbeforetoggle={beforeToggle}
    ontoggle={(event) => {
      if (event.newState === "open") {
        (panel.querySelector<HTMLInputElement>('input:checked') ?? panel.querySelector<HTMLInputElement>('input'))?.focus({ preventScroll: true });
      }
    }}
    onkeydown={onPanelKeydown}
  >
    <fieldset>
      <legend>輸出目的地 · 可多選</legend>
      {#each options as option (option.trackId)}
        <label class="destination-option">
          <input type="checkbox" checked={selected.includes(option.trackId)}
            onchange={(event) => onToggle(option.trackId, event.currentTarget.checked)} />
          <span class="destination-dot" style:background={cssColor(option.color)} aria-hidden="true"></span>
          <span class="destination-name">{option.name}</span>
        </label>
      {/each}
    </fieldset>
  </div>
</div>

<style>
  .destination-control { flex: 1; min-width: 0; }
  .destination-trigger {
    width: 100%; min-width: 0; display: flex; align-items: center; gap: 5px;
    padding: 3px 6px; font-size: 12px; line-height: normal;
    background: var(--bg); color: var(--text); border: 1px solid var(--border);
    border-radius: var(--proto-radius, 4px); box-shadow: var(--proto-inset, none); text-align: left;
  }
  .destination-trigger[aria-expanded="true"] { border-color: var(--accent); }
  .destination-trigger svg { flex: 0 0 10px; }
  .destination-summary { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .destination-count, .destination-pending { color: var(--text-dim); flex: none; }
  .destination-popup {
    position: fixed; inset: auto; margin: 0; padding: 6px; box-sizing: border-box;
    overflow: auto; background: var(--bg-panel); color: var(--text);
    border: 1px solid var(--border); border-radius: 4px;
    box-shadow: 0 8px 24px #0006; font: 12px/1.45 "IBM Plex Sans TC", "Segoe UI", system-ui, sans-serif;
  }
  .destination-popup::backdrop { background: transparent; }
  fieldset { border: 0; margin: 0; padding: 0; min-width: 0; }
  legend { padding: 3px 6px 7px; color: var(--text-dim); font-size: 11px; }
  .destination-option { display: flex; align-items: center; gap: 7px; min-height: 30px; padding: 4px 6px; border-radius: 3px; cursor: pointer; }
  .destination-option:hover, .destination-option:focus-within { background: var(--bg-raised); }
  .destination-option input { flex: none; margin: 0; width: 14px; height: 14px; accent-color: var(--accent); }
  .destination-dot { width: 7px; height: 7px; border-radius: 50%; flex: none; }
  .destination-name { min-width: 0; overflow-wrap: anywhere; }
</style>
