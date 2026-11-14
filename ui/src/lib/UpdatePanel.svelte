<script lang="ts">
  import { onMount } from "svelte";
  import { getVersion } from "@tauri-apps/api/app";
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { check as checkPluginUpdate, type Update } from "@tauri-apps/plugin-updater";
  import { checkRelease, type ReleaseUpdate } from "./updates";

  let { visible, autoCheck, engineVersion = "", onShow, onPreference, onAvailable }: {
    visible: boolean;
    autoCheck: boolean | undefined;
    /** 引擎連線版本；空字串 = 尚未連線 */
    engineVersion?: string;
    onShow: () => void;
    onPreference: (enabled: boolean) => Promise<boolean>;
    onAvailable: (version: string) => void;
  } = $props();
  let version = $state("");
  let checking = $state(false);
  let saving = $state(false);
  let opening = $state(false);
  let message = $state("尚未檢查更新");
  let error = $state("");
  let update = $state<ReleaseUpdate | null>(null);
  let portable = $state(true);  // 預設保守:未知時顯示瀏覽器下載(不會誤觸 NSIS 更新)
  // 立即更新狀態機:idle → checking → downloading(進度) → installing → error
  let installPhase = $state<"idle" | "checking" | "downloading" | "installing" | "error">("idle");
  let downloadProgress = $state("");
  let downloadedBytes = 0;   // 累計 chunk(非 $state:不觸發 re-render,進度字串才觸發)
  let totalBytes = 0;
  let startupHandled = false;
  let disposed = false;
  let versionPromise: Promise<string> | null = null;

  function loadVersion() {
    versionPromise ??= getVersion().then((value) => (version = value)).catch((reason) => {
      versionPromise = null;
      throw reason;
    });
    return versionPromise;
  }

  /** 下載進度:有總長顯示 %,無總長顯示已下載 MB */
  function progressText(downloaded: number, total: number): string {
    if (total > 0) return `${Math.min(100, Math.round((downloaded / total) * 100))}%`;
    return `${(downloaded / 1048576).toFixed(1)} MB`;
  }

  async function installUpdate() {
    if (installPhase !== "idle") return;
    installPhase = "checking";
    error = "";
    downloadProgress = "";
    try {
      const latest = await checkPluginUpdate();
      if (!latest) {
        message = "目前已是最新版本";
        installPhase = "idle";
        return;
      }
      installPhase = "downloading";
      downloadedBytes = 0;
      totalBytes = 0;
      await latest.downloadAndInstall((event) => {
        if (event.event === "Started") {
          totalBytes = event.data.contentLength ?? 0;
          downloadProgress = progressText(0, totalBytes);
        } else if (event.event === "Progress") {
          downloadedBytes += event.data.chunkLength;
          downloadProgress = progressText(downloadedBytes, totalBytes);
        } else if (event.event === "Finished") {
          installPhase = "installing";
        }
      });
      // Windows 上 NSIS /R 會殺掉本程序並重啟新版;走到這裡 = fallback
      message = "更新完成，請重新啟動程式";
      installPhase = "idle";
    } catch (reason) {
      if (disposed) return;
      installPhase = "error";
      error = reason instanceof Error ? reason.message : String(reason);
    }
  }

  async function check(automatic = false) {
    if (checking) return;
    checking = true;
    error = "";
    update = null;
    message = "正在檢查更新…";
    try {
      const result = await checkRelease(await loadVersion());
      if (disposed) return;
      update = result;
      message = result.available ? `有新版本 v${result.version} 可供下載` : "目前已是最新版本";
      if (automatic && result.available) onAvailable(result.version);
    } catch (reason) {
      if (disposed) return;
      message = "檢查更新失敗";
      error = reason instanceof Error ? reason.message : String(reason);
    } finally {
      checking = false;
    }
  }

  async function changePreference(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    if (saving) return;
    saving = true;
    const requested = input.checked;
    try {
      if (!await onPreference(requested)) input.checked = autoCheck ?? false;
    } finally {
      saving = false;
    }
  }

  async function openDownload() {
    opening = true;
    error = "";
    try {
      await invoke("open_release_page");
    } catch {
      error = "無法開啟瀏覽器";
    } finally {
      opening = false;
    }
  }

  $effect(() => {
    if (!startupHandled && autoCheck !== undefined && version) {
      startupHandled = true;
      if (autoCheck) void check(true);
    }
  });

  onMount(() => {
    disposed = false;
    let unlisten: (() => void) | undefined;
    void listen<boolean>("tray-about-requested", (event) => {
      onShow();
      if (event.payload) void check();
    }).then((cleanup) => {
      if (disposed) cleanup();
      else unlisten = cleanup;
    }).catch(() => { error = "系統匣事件無法連線；可手動檢查"; });
    void loadVersion().catch(() => { error = "無法讀取版本"; });
    invoke<{ portable: boolean }>("updater_state").then(
      (state) => { portable = state.portable; },
    ).catch(() => { /* 維持保守預設:走瀏覽器下載 */ });
    return () => { disposed = true; unlisten?.(); };
  });
</script>

<section hidden={!visible} aria-label="關於與更新">
  <h2>版本</h2>
  <div class="setrow">
    <div class="setrow-info"><span class="setrow-name">應用程式</span></div>
    <div class="setrow-ctrl"><span class="setrow-value">{version ? `v${version}` : "讀取中…"}</span></div>
  </div>
  <div class="setrow">
    <div class="setrow-info"><span class="setrow-name">Audio Engine</span></div>
    <div class="setrow-ctrl">
      <span class="setrow-value">{engineVersion || "未連線"}</span>
    </div>
  </div>
  <h2>更新</h2>
  <div class="setrow">
    <div class="setrow-info">
      <span
        class="setrow-name"
        data-tooltip={portable
          ? "便攜版：瀏覽器下載新版 zip。"
          : "安裝版：自動安裝並重啟。"}>更新狀態</span
      >
    </div>
    <div class="setrow-ctrl">
      <button disabled={checking} onclick={() => check()}>{checking ? "檢查中…" : "檢查更新"}</button>
      {#if update?.available && !portable}
        <button class="primary" disabled={installPhase !== "idle"} onclick={installUpdate}>
          {installPhase === "checking" ? "確認中…"
            : installPhase === "downloading" ? `下載中 ${downloadProgress}`
            : installPhase === "installing" ? "安裝中，即將重啟…"
            : installPhase === "error" ? "重試更新"
            : "立即更新"}
        </button>
      {:else if update?.available}
        <button class="primary" disabled={opening} onclick={openDownload}>{opening ? "開啟中…" : "前往下載新版"}</button>
      {/if}
    </div>
  </div>
  <div class="update-status" aria-live="polite" aria-busy={checking}>
    <p>{message}</p>
    {#if error}<p class="error" role="alert">{error}</p>{/if}
  </div>
  <div class="setrow" class:disabled={saving || autoCheck === undefined}>
    <div class="setrow-info">
      <label
        class="setrow-name"
        for="autocheck"
        data-tooltip="啟動時背景檢查。">自動檢查更新</label
      >
    </div>
    <div class="setrow-ctrl">
      <input id="autocheck" type="checkbox" checked={autoCheck ?? false} disabled={saving || autoCheck === undefined}
        onchange={changePreference} />
    </div>
  </div>
</section>

<style>
  section { padding: 0; }
  section[hidden] { display: none; }
  h2 { margin: 2px 0 0; font-size: 12px; color: var(--text-dim); display: flex; align-items: center; gap: 8px; }
  /* 同 settingsdlg h2：類別後接髮絲線（scoped 樣式無法跨元件,故重複） */
  h2::after { content: ""; flex: 1; height: 1px; background: var(--border); }
  h2:not(:first-child) { margin-top: 16px; }
  .update-status { margin: 0; }
  .update-status p { margin: 0; font-size: 12px; color: var(--text-dim); }
  .update-status .error { margin-top: 2px; color: var(--err); overflow-wrap: anywhere; }
</style>
