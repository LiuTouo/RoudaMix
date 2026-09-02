<script lang="ts">
  // P1-C:App 軌程序選擇器。session 載入後 app 軌 pid=0(needsRebind)—— engine
  // 不猜 PID,由使用者在此選。列正在出聲的 active audio sessions(exe 名、PID、
  // 完整路徑);同名程序可辨識。重新整理 + 搜尋;空清單說明原因。
  import { engineCommand } from "./protocol-commands.generated";
  import { friendlyError } from "./errors";
  import type { AudioApp } from "./types";

  let {
    trackName,
    savedName,
    onPick,
    onClose,
  }: {
    trackName: string;
    /** session 記錄的來源名(顯示「原本是這個程式」) */
    savedName?: string | null;
    onPick: (pid: number, name: string) => void;
    onClose: () => void;
  } = $props();

  let apps = $state<AudioApp[]>([]);
  let filter = $state("");
  let err = $state("");
  let loading = $state(false);
  let dlg = $state<HTMLDialogElement | null>(null);

  const shown = $derived(
    apps.filter(
      (a) =>
        filter === "" ||
        a.name.toLowerCase().includes(filter.toLowerCase()) ||
        (a.path ?? "").toLowerCase().includes(filter.toLowerCase()),
    ),
  );

  async function refresh() {
    loading = true;
    err = "";
    try {
      const r = await engineCommand("list_audio_apps", {});
      apps = (r.apps as AudioApp[]) ?? [];
    } catch (e) {
      err = friendlyError(String(e)).friendly;
    }
    loading = false;
  }

  $effect(() => {
    if (dlg && !dlg.open) {
      dlg.showModal();
      void refresh(); // 開啟即拉一次
    }
  });

  function pick(a: AudioApp) {
    onPick(a.pid, a.name);
    onClose();
  }
  function dirOf(p: string): string {
    const parts = p.split(/[\\/]/);
    parts.pop();
    return parts.join("\\");
  }
</script>

<dialog
  bind:this={dlg}
  class="pickerdlg"
  onclose={onClose}
  aria-label="選擇要捕捉的程式"
>
  <div class="cardhead dialog-head">
    <span class="dialog-title">選擇程式 — 「{trackName}」</span>
    <button onclick={() => void refresh()} disabled={loading}
      >{loading ? "整理中…" : "重新整理"}</button
    >
    <button
      class="dialog-close"
      type="button"
      aria-label="關閉程序選擇器"
      onclick={onClose}
      data-tooltip="關閉程序選擇器，不變更目前綁定。">×</button
    >
  </div>
  {#if savedName}
    <p class="dim saved">
      Session 記錄的來源:<span class="mono">{savedName}</span
      >{#if apps.some((a) => a.name === savedName)}
        — 清單內有同名程序,請用路徑/PID 確認
      {/if}
    </p>
  {/if}
  <div class="searchrow">
    <label class="dim" for="appfilter">搜尋</label>
    <input id="appfilter" type="search" bind:value={filter} placeholder="名稱或路徑" />
  </div>
  {#if err}
    <p class="err mono" data-tooltip={`無法取得程序清單：${err}`}>{err}</p>
  {:else if shown.length === 0}
    <p class="dim empty">
      {apps.length === 0
        ? "目前沒有正在出聲的程式。清單只列「正在播放音訊」的作用中工作階段 —— 在目標程式裡播放一段聲音,再按「重新整理」。"
        : "沒有符合搜尋的程式。"}
    </p>
  {:else}
    <div class="applist">
      {#each shown as a (a.pid)}
        <button class="app" onclick={() => pick(a)}>
          <span class="appname">{a.name}</span>
          <span
            class="apppath mono"
            data-tooltip={a.path ? `執行檔：\n${a.path}\nPID ${a.pid}` : `PID ${a.pid}`}
            >{a.path ? dirOf(a.path) : ""} · PID {a.pid}</span
          >
        </button>
      {/each}
    </div>
  {/if}
</dialog>

<style>
  .pickerdlg {
    background: var(--bg-panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    width: min(520px, 92vw);
  }
  /* P1-K:dialog 內容上限 85vh,超出垂直捲動;關閉鈕不被裁 */
  .pickerdlg[open] {
    display: flex;
    flex-direction: column;
    gap: 10px;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    margin: 0;
    max-height: 85vh;
  }
  .pickerdlg::backdrop {
    background: rgb(0 0 0 / 0.5);
  }
  .cardhead {
    display: flex;
    align-items: center;
    gap: 8px;
    font-weight: 600;
    font-size: 13px;
  }
  .saved {
    margin: 0;
    font-size: 12.5px;
  }
  .searchrow {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .searchrow input {
    flex: 1;
    background: var(--bg);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 5px 8px;
    font-size: 13px;
  }
  .applist {
    display: flex;
    flex-direction: column;
    gap: 6px;
    overflow-y: auto;
    min-height: 80px;
  }
  .app {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 2px;
    padding: 7px 10px;
    text-align: left;
  }
  .app:hover {
    border-color: var(--accent);
  }
  .appname {
    font-size: 13px;
    font-weight: 600;
  }
  .apppath {
    font-size: 10.5px;
    color: var(--text-dim);
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    user-select: text; /* P1-O:路徑可選取複製 */
  }
  .empty {
    margin: 4px 0;
  }
  .dim {
    color: var(--text-dim);
  }
  .mono {
    font-family: var(--mono);
  }
  .err {
    color: var(--warn);
    margin: 0;
    user-select: text;
  }
</style>
