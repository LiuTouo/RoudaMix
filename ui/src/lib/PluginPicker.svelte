<script lang="ts">
  import { onMount } from "svelte";
  import { friendlyError, type FriendlyError } from "./errors";
  import { buildPluginCatalog, browsePlugins, type PluginGrouping, type CatalogPlugin } from "./pluginCatalog";
  import type { ScanModule, ScanFailure } from "./types";

  let {
    trackName, modules, failures = [], scanRunning = false, scanProgress = null,
    scanNotice = "", onPick, onCancelScan, onClose,
  }: {
    trackName: string;
    modules: ScanModule[];
    failures?: ScanFailure[];
    scanRunning?: boolean;
    scanProgress?: { done: number; total: number } | null;
    scanNotice?: string;
    onPick: (path: string, classId: string) => Promise<unknown>;
    onCancelScan: () => void;
    onClose: () => void;
  } = $props();

  let dialog: HTMLDialogElement;
  let search: HTMLInputElement;
  let query = $state("");
  let grouping = $state<PluginGrouping>("vendor");
  let pending = $state(false);
  let error = $state<FriendlyError | null>(null);
  let disposed = false;
  const catalog = $derived(buildPluginCatalog(modules));
  const result = $derived(browsePlugins(catalog, query, grouping));

  onMount(() => {
    const opener = document.activeElement;
    dialog.showModal();
    search.focus();
    return () => {
      disposed = true;
      dialog.close();
      if (opener instanceof HTMLElement && opener.isConnected) opener.focus();
    };
  });

  async function pick(plugin: CatalogPlugin) {
    if (pending) return;
    pending = true;
    error = null;
    try {
      await onPick(plugin.path, plugin.classId);
      if (!disposed) dialog.close();
    } catch (e) {
      if (!disposed) error = friendlyError(e);
    } finally {
      if (!disposed) pending = false;
    }
  }
</script>

<dialog bind:this={dialog} class="plugin-picker" aria-label={`VST 插件列表 — 加入「${trackName}」`} onclose={() => { if (!disposed) onClose(); }}>
  <div class="dialog-head">
    <span class="dialog-title">VST 插件列表 — 加入「{trackName}」</span>
    <button class="dialog-close" type="button" aria-label="關閉插件選擇器"
      data-tooltip="關閉插件選擇器，不加入其他項目。" onclick={() => dialog.close()}>×</button>
  </div>
  <div class="toolbar">
    <label class="search-field">
      <span>搜尋插件</span>
      <input bind:this={search} type="search" bind:value={query} placeholder="名稱、廠牌或分類，可輸入多個關鍵字" />
    </label>
    <button class="clear" type="button" disabled={!query}
      onclick={() => { query = ""; search.focus(); }}>清除搜尋</button>
    <label class="grouping-field">
      <span>分類方式</span>
      <select bind:value={grouping}>
        <option value="vendor">廠牌</option>
        <option value="type">效果類型</option>
      </select>
    </label>
  </div>
  <div class="statusline">
    <span role="status">{result.count} / {catalog.length} 個插件</span>
    {#if pending}<span>正在加入插件…</span>{/if}
    {#if scanRunning}
      <span>背景掃描中{scanProgress ? ` ${scanProgress.done}/${scanProgress.total}` : "…"}</span>
      <button type="button" onclick={onCancelScan}>取消掃描</button>
    {/if}
  </div>
  {#if error}
    <div class="error" role="alert">
      <p>{error.friendly}</p>
      <details><summary>技術詳細資訊</summary><p>{error.raw}</p></details>
    </div>
  {/if}
  {#if scanNotice}<p class="notice">{scanNotice}</p>{/if}
  <div class="results" aria-label="插件搜尋結果" aria-busy={pending}>
    {#if catalog.length === 0}
      <p class="empty">{scanRunning ? "正在掃描 VST，完成後會顯示可加入的插件。" : "尚無 VST 清單 — 請按頂欄「掃描 VST」"}</p>
    {:else if result.count === 0}
      <p class="empty">找不到符合條件的插件，請更換關鍵字或清除搜尋。</p>
    {:else}
      {#each result.groups as group (group.name)}
        <section aria-label={group.name}>
          <h2>{group.name}<span>{group.plugins.length}</span></h2>
          <ul>
            {#each group.plugins as plugin (plugin.key)}
              <li>
                <button class="plugin-row" type="button" disabled={pending}
                  onclick={() => void pick(plugin)}
                  data-tooltip={`${plugin.path}\n${plugin.vendor} · ${plugin.version}`}>
                  <span class="identity"><strong>{plugin.name}</strong><span class="source">{plugin.source}</span></span>
                  <span class="vendor">{plugin.vendor}</span>
                  <span class="categories">{#each plugin.categories as category}<span>{category}</span>{/each}</span>
                  <span class="add-label">加入</span>
                </button>
              </li>
            {/each}
          </ul>
        </section>
      {/each}
    {/if}
    {#if failures.length > 0}
      <details class="quarantine">
        <summary>無法載入 ({failures.length}) — 已隔離</summary>
        {#each failures as failure}
          <p data-tooltip={`掃描失敗：${failure.error}\n${failure.path}`}>{failure.path}：{failure.error}</p>
        {/each}
      </details>
    {/if}
  </div>
</dialog>

<style>
  .plugin-picker {
    box-sizing: border-box;
    width: min(1040px, 92vw);
    height: min(760px, 88dvh);
    max-width: 92vw;
    max-height: 88dvh;
    padding: 20px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-panel);
    color: var(--text);
    overflow: hidden;
  }
  .plugin-picker[open] { display: flex; flex-direction: column; gap: 14px; }
  .plugin-picker::backdrop { background: rgb(0 0 0 / 0.55); }
  .dialog-head, .toolbar, .statusline { flex-shrink: 0; }
  .dialog-title { font-size: 17px; overflow-wrap: anywhere; }
  .toolbar { display: flex; flex-wrap: wrap; align-items: flex-end; gap: 12px; }
  label { display: flex; flex-direction: column; gap: 6px; color: var(--text-dim); font-size: 12px; }
  .search-field { flex: 1 1 320px; min-width: 0; }
  .grouping-field { flex: 0 1 150px; min-width: 0; }
  input, select { box-sizing: border-box; min-width: 0; width: 100%; height: 36px; padding: 7px 10px; border: 1px solid var(--border); border-radius: 5px; background: var(--bg); color: var(--text); font: inherit; }
  .clear { height: 36px; }
  .statusline { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; font-size: 12px; color: var(--text-dim); }
  .statusline > :first-child { margin-right: auto; }
  .results { flex: 1; min-height: 0; overflow-y: auto; overflow-x: hidden; scrollbar-gutter: stable; }
  section + section { margin-top: 20px; }
  h2 { display: flex; align-items: center; gap: 8px; margin: 0 0 8px; font-size: 13px; color: var(--accent); }
  h2 span { color: var(--text-dim); font-size: 11px; font-weight: normal; }
  ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 5px; }
  .plugin-row { width: 100%; display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr) minmax(0, 1.2fr) auto; align-items: center; gap: 14px; text-align: left; padding: 11px 12px; }
  .plugin-row > * { min-width: 0; overflow-wrap: anywhere; }
  .identity { display: flex; flex-direction: column; gap: 4px; }
  strong { font-size: 14px; font-weight: 600; }
  .source { font-size: 11px; color: var(--text-dim); }
  .vendor { font-size: 12px; }
  .categories { display: flex; flex-wrap: wrap; gap: 4px; }
  .categories span { font-size: 11px; padding: 2px 6px; border: 1px solid var(--border); border-radius: 4px; }
  .add-label { font-size: 12px; color: var(--accent); }
  .empty { padding: 28px 10px; text-align: center; color: var(--text-dim); }
  .error, .notice { margin: 0; flex-shrink: 0; max-height: 72px; overflow: auto; overflow-wrap: anywhere; font-size: 12px; }
  .error { color: var(--err); }
  .error p { margin: 0; }
  .notice { color: var(--warn); }
  .quarantine { margin-top: 20px; color: var(--text-dim); font-size: 12px; overflow-wrap: anywhere; }
  summary { cursor: pointer; }
  @media (max-width: 620px) {
    .plugin-picker { padding: 12px; gap: 10px; }
    .search-field { flex-basis: 100%; }
    .grouping-field { flex-grow: 1; }
    .plugin-row { grid-template-columns: minmax(0, 1fr) auto; gap: 8px; }
    .identity { grid-column: 1 / -1; }
    .categories { grid-column: 1; }
    .add-label { grid-column: 2; grid-row: 2 / 4; }
  }
</style>
