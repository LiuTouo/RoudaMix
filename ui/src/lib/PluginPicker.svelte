<script lang="ts">
  import { onMount } from "svelte";
  import { friendlyError, type FriendlyError } from "./errors";
  import { backdropClose } from "./backdropClose";
  import { buildPluginCatalog, browseTree, catalogCategories, type CatalogPlugin } from "./pluginCatalog";
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
  let category = $state("");
  let selected = $state<string | null>(null);
  let pending = $state(false);
  let error = $state<FriendlyError | null>(null);
  let disposed = false;
  const catalog = $derived(buildPluginCatalog(modules));
  const categories = $derived(catalogCategories(catalog));
  // 重掃後選定的分類可能已不存在；失效時視為全部分類。
  const activeCategory = $derived(categories.includes(category) ? category : "");
  // 樹狀目錄：廠牌 → 插件；搜尋時全部展開，平時全部收合。
  const tree = $derived(browseTree(catalog, query, activeCategory));

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
    <label class="category-field">
      <span>分類</span>
      <select bind:value={category} data-tooltip="僅顯示選定分類的插件；清單來自掃描到的插件 metadata。">
        <option value="">全部分類</option>
        {#each categories as item (item)}<option value={item}>{item}</option>{/each}
      </select>
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
    <span role="status">{tree.count} / {catalog.length} 個插件</span>
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
    {:else if tree.count === 0}
      <p class="empty">找不到符合條件的插件，請更換關鍵字、分類或清除搜尋。</p>
    {:else}
      {#each tree.vendors as vendor (vendor.name)}
        <details class="plugin-group" open={query !== ""} aria-label={vendor.name}>
          <summary class:sel={selected === `v:${vendor.name}`} onclick={() => (selected = `v:${vendor.name}`)}>
            <span class="chev" aria-hidden="true"></span>
            <span class="tree-label">{vendor.name}</span>
            <span class="tree-count">{vendor.plugins.length}</span>
          </summary>
          <div class="kids">
            <ul>
              {#each vendor.plugins as plugin (vendor.name + "/" + plugin.key)}
                <li>
                  <button class="plugin-row" type="button" disabled={pending}
                    class:sel={selected === plugin.key}
                    onclick={() => { selected = plugin.key; void pick(plugin); }}
                    data-tooltip={`${plugin.path}\n${plugin.vendor} · ${plugin.version}`}>
                    <span class="tree-label">{plugin.name}</span>
                    <span class="source">{plugin.source}</span>
                    <span class="cats">{#each plugin.categories as item (item)}<span>{item}</span>{/each}</span>
                  </button>
                </li>
              {/each}
            </ul>
          </div>
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
  <!-- 置中進度條：掃描有進度時顯示比例，其餘顯示循環動畫；不擋瀏覽與取消 -->
  {#if scanRunning || pending}
    <div class="busy-veil" role="status">
      {#if scanRunning}
        <div class="busy-box">
          <h3>正在掃描 VST…</h3>
          {#if scanProgress && scanProgress.total > 0}
            <div class="busy-track" aria-label="掃描進度 {scanProgress.done}/{scanProgress.total}">
              <div class="busy-fill" style="width:{Math.round((scanProgress.done / scanProgress.total) * 100)}%"></div>
            </div>
            <p class="busy-count">{scanProgress.done}/{scanProgress.total}</p>
          {:else}
            <div class="busy-track"><div class="busy-fill indeterminate"></div></div>
          {/if}
        </div>
      {/if}
      {#if pending}
        <div class="busy-box">
          <h3>正在加入插件…</h3>
          <div class="busy-track"><div class="busy-fill indeterminate"></div></div>
        </div>
      {/if}
    </div>
  {/if}
</dialog>

<style>
  .plugin-picker {
    box-sizing: border-box;
    width: min(560px, 92vw);
    /* 高度隨內容收放；清單短時不留大片空白。fit-content：top-layer dialog 的
     * UA inset:0 會拉伸 auto 高度，必須顯式指定 */
    height: fit-content;
    min-height: 320px;
    max-width: 92vw;
    max-height: min(760px, 88dvh);
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
  .search-field { flex: 1 1 220px; min-width: 0; }
  .category-field select { height: 36px; min-width: 96px; }
  /* 邊框/底色/圓角由 console.css 的對話框欄位語彙統一（下劃線式） */
  input { box-sizing: border-box; min-width: 0; width: 100%; height: 36px; padding: 7px 10px; font: inherit; }
  .clear { height: 36px; }
  .scan { height: 36px; }
  .statusline { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; font-size: 12px; color: var(--text-dim); }
  .statusline > :first-child { margin-right: auto; }
  .results { flex: 1; min-height: 0; overflow-y: auto; overflow-x: hidden; scrollbar-gutter: stable; padding: 2px 0 8px; }

  /* ---- 樹狀目錄 ---- */
  summary { cursor: pointer; }
  .plugin-group { margin-top: 2px; }
  .plugin-group summary {
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
  ul { list-style: none; margin: 0; padding: 2px 0 6px; display: flex; flex-direction: column; gap: 2px; }
  .plugin-row {
    display: flex; width: 100%; align-items: center; gap: 8px;
    text-align: left; padding: 5px 8px; font-size: 13px; color: var(--text);
  }
  .plugin-row .tree-label { flex: unset; }
  .source { flex: none; max-width: 38%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 11px; color: var(--text-dim); }
  .cats { display: flex; flex: none; gap: 4px; }
  .cats span { font-size: 10px; padding: 1px 6px; border: 1px solid var(--border); border-radius: var(--flat-radius); color: var(--text-dim); }
  .sel :is(.source, .cats span) { color: inherit; border-color: color-mix(in srgb, var(--flat-0) 40%, transparent); }
  /* 反白選取；!important 壓過全域主題對 .plugin-row/summary 的底色設定 */
  :global(#app) .sel {
    background: var(--accent) !important; border-color: var(--accent); color: var(--flat-0) !important;
  }
  .sel :is(.tree-label, .tree-count) { color: inherit; opacity: 0.75; }
  .sel .tree-label { opacity: 1; }

  /* ---- 置中進度條（掃描/加入中） ---- */
  /* 對話框本身由 top layer 置中，fixed + 50% 即為視窗／對話框中央 */
  .busy-veil {
    position: fixed;
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
    z-index: 10;
    display: flex;
    flex-direction: column;
    gap: 10px;
    pointer-events: none; /* 背景掃描仍可瀏覽清單、按取消 */
  }
  .busy-box {
    min-width: 260px;
    max-width: min(360px, 80vw);
    padding: 14px 16px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--flat-2);
    box-shadow: 0 18px 44px #000b;
    text-align: center;
  }
  .busy-box h3 { margin: 0 0 10px; font-size: 14px; }
  .busy-count { margin: 8px 0 0; color: var(--text-dim); font-size: 12px; text-align: right; }
  .busy-track { height: 6px; border-radius: 3px; background: var(--border); overflow: hidden; }
  .busy-fill { height: 100%; background: var(--accent); transition: width 120ms linear; }
  .busy-fill.indeterminate { width: 30%; animation: busy-slide 1.2s infinite linear; }
  @keyframes busy-slide { from { transform: translateX(-110%); } to { transform: translateX(380%); } }

  .empty { padding: 28px 10px; text-align: center; color: var(--text-dim); }
  .error, .notice { margin: 0; flex-shrink: 0; max-height: 72px; overflow: auto; overflow-wrap: anywhere; font-size: 12px; }
  .error { color: var(--err); }
  .error p { margin: 0; }
  .notice { color: var(--warn); }
  .quarantine { margin-top: 20px; color: var(--text-dim); font-size: 12px; overflow-wrap: anywhere; }
  @media (max-width: 620px) {
    .plugin-picker { padding: 12px; gap: 10px; }
    .search-field { flex-basis: 100%; }
    .cats { display: none; }
  }
</style>
