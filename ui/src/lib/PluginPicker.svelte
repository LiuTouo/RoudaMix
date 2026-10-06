<script lang="ts">
  import { onMount } from "svelte";
  import { friendlyError, type FriendlyError } from "./errors";
  import { backdropClose } from "./backdropClose";
  import { buildPluginCatalog, browsePlugins, type CatalogPlugin } from "./pluginCatalog";
  import type { ScanModule, ScanFailure } from "./types";

  let {
    trackName, modules, failures = [], scanRunning = false, scanProgress = null,
    scanNotice = "", onStartScan, scanReady = false, onPick, onCancelScan, onClose,
  }: {
    trackName: string;
    modules: ScanModule[];
    failures?: ScanFailure[];
    scanRunning?: boolean;
    scanProgress?: { done: number; total: number } | null;
    scanNotice?: string;
    onStartScan: () => Promise<boolean>;
    scanReady?: boolean;
    onPick: (path: string, classId: string) => Promise<unknown>;
    onCancelScan: () => void;
    onClose: () => void;
  } = $props();

  let dialog: HTMLDialogElement;
  let search: HTMLInputElement;
  let query = $state("");
  let sort = $state<{ key: SortKey; dir: 1 | -1 }>({ key: "name", dir: 1 });
  let pending = $state(false);
  let error = $state<FriendlyError | null>(null);
  let disposed = false;
  const catalog = $derived(buildPluginCatalog(modules));

  type SortKey = "name" | "source" | "type";
  const sortColumns: Array<{ key: SortKey; label: string }> = [
    { key: "name", label: "名稱" }, { key: "source", label: "來源" }, { key: "type", label: "類型" },
  ];
  const collator = new Intl.Collator("zh-TW", { numeric: true, sensitivity: "base" });
  const sortValue = (plugin: CatalogPlugin, key: SortKey) =>
    key === "type" ? plugin.categories.join(" ") : plugin[key];
  // 固定廠牌分組；排序只影響各組內的列，組序永遠按廠牌名（browsePlugins）。
  const result = $derived.by(() => {
    const base = browsePlugins(catalog, query);
    return {
      count: base.count,
      groups: base.groups.map((group) => ({
        ...group,
        plugins: [...group.plugins].sort((a, b) =>
          sort.dir * collator.compare(sortValue(a, sort.key), sortValue(b, sort.key)) || collator.compare(a.name, b.name)),
      })),
    };
  });
  function toggleSort(key: SortKey) {
    sort = sort.key === key ? { key, dir: sort.dir === 1 ? -1 : 1 } : { key, dir: 1 };
  }

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

<dialog bind:this={dialog} class="plugin-picker" use:backdropClose={() => dialog.close()} aria-label={`VST 插件列表 — 加入「${trackName}」`} onclose={() => { if (!disposed) onClose(); }}>
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
    {#if scanRunning}
      <button class="scan" type="button" onclick={onCancelScan}
        data-tooltip="取消目前的背景 VST 掃描；取消後會保留掃描前的 plugin 清單。">取消掃描</button>
    {:else}
      <button class="scan" type="button" disabled={!scanReady} onclick={() => void onStartScan()}
        data-tooltip="在背景掃描預設 VST3 目錄；未變更的 plugin 會沿用持久快取，不重新載入。">掃描 VST</button>
    {/if}
  </div>
  <div class="statusline">
    <span role="status">{result.count} / {catalog.length} 個插件</span>
    {#if pending}<span>正在加入插件…</span>{/if}
    {#if scanRunning}
      <span>背景掃描中{scanProgress ? ` ${scanProgress.done}/${scanProgress.total}` : "…"}</span>
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
      <p class="empty">{scanRunning ? "正在掃描 VST，完成後會顯示可加入的插件。" : "尚無 VST 清單 — 請按上方「掃描 VST」"}</p>
    {:else if result.count === 0}
      <p class="empty">找不到符合條件的插件，請更換關鍵字或清除搜尋。</p>
    {:else}
      <div class="plugin-thead">
        {#each sortColumns as column (column.key)}
          <button class="sort" type="button" class:active={sort.key === column.key}
            onclick={() => toggleSort(column.key)}
            aria-label={`依${column.label}排序${sort.key === column.key && sort.dir === -1 ? "（目前：遞減）" : ""}`}
            data-tooltip={`依${column.label}排序；再按一次反向。`}>
            {column.label}{sort.key === column.key ? (sort.dir === 1 ? " ▲" : " ▼") : ""}
          </button>
        {/each}
        <span class="add-label" aria-hidden="true"></span>
      </div>
      {#each result.groups as group (group.name)}
        <details class="plugin-group" open={query !== ""} aria-label={group.name}>
          <summary>
            <h2>{group.name}<span>{group.plugins.length}</span></h2>
          </summary>
          <ul>
            {#each group.plugins as plugin (plugin.key)}
              <li>
                <button class="plugin-row" type="button" disabled={pending}
                  onclick={() => void pick(plugin)}
                  data-tooltip={`${plugin.path}\n${plugin.vendor} · ${plugin.version}`}>
                  <span class="name"><strong>{plugin.name}</strong></span>
                  <span class="source">{plugin.source}</span>
                  <span class="categories">{#each plugin.categories as category}<span>{category}</span>{/each}</span>
                  <span class="add-label">加入</span>
                </button>
              </li>
            {/each}
          </ul>
        </details>
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
    padding: 14px 16px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--bg-panel);
    color: var(--text);
    overflow: hidden;
    box-shadow: 0 8px 24px rgb(0 0 0 / 0.55);
  }
  .plugin-picker[open] { display: flex; flex-direction: column; gap: 14px; }
  .plugin-picker::backdrop { background: rgb(0 0 0 / 0.5); }
  .dialog-head, .toolbar, .statusline { flex-shrink: 0; }
  .plugin-picker .dialog-title { overflow-wrap: anywhere; }
  .toolbar { display: flex; flex-wrap: wrap; align-items: flex-end; gap: 12px; }
  label { display: flex; flex-direction: column; gap: 6px; color: var(--text-dim); font-size: 12px; }
  .search-field { flex: 1 1 320px; min-width: 0; }
  input { box-sizing: border-box; min-width: 0; width: 100%; height: 36px; padding: 7px 10px; border: 1px solid var(--border); border-radius: 6px; background: var(--bg); color: var(--text); font: inherit; }
  .clear { height: 36px; }
  .scan { height: 36px; }
  .statusline { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; font-size: 12px; color: var(--text-dim); }
  .statusline > :first-child { margin-right: auto; }
  .results { flex: 1; min-height: 0; overflow-y: auto; overflow-x: hidden; scrollbar-gutter: stable; }
  .plugin-thead, .plugin-row { width: 100%; display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1.2fr) minmax(0, 1fr) auto; align-items: center; gap: 10px; }
  .plugin-thead {
    position: sticky; top: 0; z-index: 1; padding: 4px 12px;
    font-size: 12px; color: var(--text-dim);
    background: var(--bg-panel); border-bottom: 1px solid var(--border);
  }
  .plugin-thead .sort { padding: 0; font: inherit; color: var(--text-dim); text-align: left; background: none; border: 0; cursor: pointer; }
  .plugin-thead .sort:hover, .plugin-thead .sort.active { color: var(--text); }
  .plugin-thead .sort:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  .plugin-group + .plugin-group, .plugin-thead + .plugin-group { margin-top: 8px; }
  summary { cursor: pointer; }
  .plugin-group summary {
    display: flex; align-items: center; gap: 8px; padding: 3px 6px;
    list-style: none; border-radius: 4px;
  }
  .plugin-group summary::-webkit-details-marker { display: none; }
  .plugin-group summary::before { content: "▸"; color: var(--text-dim); font-size: 11px; }
  .plugin-group[open] summary::before { content: "▾"; }
  .plugin-group summary:hover { background: color-mix(in srgb, var(--bg-raised) 55%, transparent); }
  .plugin-group summary h2 { margin: 0; }
  h2 { display: flex; align-items: center; gap: 8px; margin: 0 0 8px; font-size: 13px; color: var(--accent); }
  h2 span { color: var(--text-dim); font-size: 11px; font-weight: normal; }
  ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
  .plugin-row { text-align: left; padding: 5px 12px; }
  .plugin-row > * { min-width: 0; }
  .name { display: flex; min-width: 0; }
  .name strong, .source { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  strong { font-size: 14px; font-weight: 600; }
  .source { font-size: 11px; color: var(--text-dim); }
  .categories { display: flex; flex-wrap: wrap; gap: 4px; }
  .categories span { font-size: 11px; padding: 2px 6px; border: 1px solid var(--border); border-radius: 4px; }
  .add-label { font-size: 12px; color: var(--accent); }
  .empty { padding: 28px 10px; text-align: center; color: var(--text-dim); }
  .error, .notice { margin: 0; flex-shrink: 0; max-height: 72px; overflow: auto; overflow-wrap: anywhere; font-size: 12px; }
  .error { color: var(--err); }
  .error p { margin: 0; }
  .notice { color: var(--warn); }
  .quarantine { margin-top: 20px; color: var(--text-dim); font-size: 12px; overflow-wrap: anywhere; }
  @media (max-width: 620px) {
    .plugin-picker { padding: 12px; gap: 10px; }
    .search-field { flex-basis: 100%; }
    .plugin-thead { display: flex; flex-wrap: wrap; gap: 8px 14px; }
    .plugin-row { grid-template-columns: minmax(0, 1fr) auto; }
    .name { grid-column: 1 / -1; }
    .source { grid-column: 1; }
    .categories { grid-column: 1; }
    .add-label { grid-column: 2; grid-row: 2 / 4; align-self: center; }
  }
</style>
